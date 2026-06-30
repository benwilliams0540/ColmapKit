# Metal SIFT Spike

This spike explores a minimal Metal-backed SIFT extractor path that can later sit
behind COLMAP's existing `FeatureExtractor` abstraction without blocking matcher
integration. It is intentionally additive only: no production extractor factory,
matcher, CMake, option, or enum wiring is changed.

Prototype:

```bash
xcrun clang++ -std=c++17 -fobjc-arc -framework Foundation -framework Metal \
  src/colmap/feature/metal_sift_spike_prototype.mm \
  -o /tmp/metal_sift_spike_prototype
/tmp/metal_sift_spike_prototype
```

The prototype builds runtime Metal compute pipelines, runs them on a synthetic
grayscale image, and prints the number of SIFT-like features plus a short
keypoint/descriptor preview. It does not depend on COLMAP headers so it remains
safe to compile outside the normal build graph.

## Existing COLMAP Contract

A production Metal extractor can be matcher-compatible if it preserves the same
observable output as the current SIFT extractors:

- `FeatureExtractor::Extract(const Bitmap&, FeatureKeypoints*, FeatureDescriptors*)`
  consumes grayscale `Bitmap` data for SIFT.
- `FeatureKeypoint` stores pixel-center coordinates, affine shape, scale, and
  orientation conventions used by `FeatureKeypoint(x, y, scale, orientation)`.
- `FeatureDescriptors::data` must be `N x 128` `uint8_t` rows with
  `FeatureExtractorType::SIFT`.
- Default descriptor normalization is RootSIFT-like
  (`SiftExtractionOptions::Normalization::L1_ROOT`) and then quantized with
  COLMAP's `512 * value` convention.
- Existing CPU SIFT limits by DoG level/scale before flattening orientation
  duplicates; SiftGPU uses its own top-count path via `-tc2`.

If these outputs match, the existing brute-force, FAISS, and LightGlue SIFT
matcher paths should not need new integration work.

## Prototype Pipeline

### Gaussian Pyramid

`metal_sift_spike_prototype.mm` stores each octave as a contiguous float
Gaussian stack and applies separable horizontal/vertical blur kernels. The
prototype downsamples between octaves on the CPU for simplicity.

Production direction:

- Keep one Metal buffer per octave stack or use reusable heap-backed buffers.
- Decide whether to use custom compute kernels or Metal Performance Shaders for
  blur. Custom kernels make SIFT parity easier; MPS can reduce maintenance.
- Apply the same initial blur, first-octave, and octave-resolution semantics as
  `SiftExtractionOptions`.

### DoG Extrema Detection

The spike computes a contiguous DoG stack and launches a 3D Metal grid over
`x, y, dog_level`. It checks the 26-neighborhood, peak threshold, and a basic
Hessian edge-response filter before appending candidates with an atomic counter.

Research gaps:

- No subpixel/subscale quadratic localization.
- No duplicate suppression across octave boundaries.
- Thresholds are only approximately aligned with VLFeat/SiftGPU.

### Orientation Assignment

The spike assigns one dominant orientation per candidate using a 36-bin gradient
histogram over the matching Gaussian level.

Research gaps:

- No smoothed histogram interpolation.
- No secondary orientations, despite COLMAP supporting
  `max_num_orientations`.
- `upright=true` is not modeled yet, though it is trivial to short-circuit to
  zero orientation.

### Descriptor Generation

The spike computes a 4x4x8 gradient histogram per keypoint in Metal, performs
standard SIFT L2 clipping/renormalization in the kernel, then returns float
descriptors to COLMAP's existing L2/RootSIFT normalization and `uint8_t`
quantization pipeline.

Research gaps:

- Descriptor binning uses nearest spatial/orientation bins, not full SIFT
  trilinear interpolation.
- Descriptor ordering has not been verified against COLMAP's UBC convention.
- Final normalization/quantization still happens after CPU readback; production
  should validate parity before deciding whether to move it onto the GPU.

### Keypoint Limiting

The spike limits after descriptor readback by partial-sorting on descending
scale and then absolute DoG response. This keeps the behavior close to COLMAP's
"larger-scale features" intent, but it is not a byte-for-byte match for either
VLFeat CPU SIFT or SiftGPU.

Production direction:

- Limit before descriptor generation to avoid wasted descriptor work.
- Preserve COLMAP's scale-level semantics for CPU parity, or explicitly choose
  SiftGPU-style top response/level behavior and document the difference.
- Keep all orientation duplicates for retained base keypoints until
  `max_num_orientations` is applied.

## Production-Ready vs Research-Grade

Production-ready from this spike:

- A clear Metal buffer layout for Gaussian and DoG stacks.
- Kernel decomposition for blur, DoG, extrema, orientation, and descriptors.
- Confirmation that the final output can be shaped as SIFT-compatible
  `N x 128` `uint8_t` descriptors.
- A path that does not require matcher changes if descriptors remain tagged as
  `FeatureExtractorType::SIFT`.

Research-grade only:

- Feature localization accuracy.
- Descriptor parity with VLFeat/SiftGPU.
- Orientation multiplicity and histogram smoothing.
- Exact `SiftExtractionOptions` coverage.
- Runtime scheduling, memory reuse, and multi-threaded extractor behavior.

## Current Hardware Validation

On 2026-06-30, `mise run compare:gerrard-extractor` was run on Apple Silicon
with hardware Metal access against the Gerrard Hall dataset, using
`COLMAP_COMPARE_MAX_IMAGE_SIZE=1000`, `COLMAP_COMPARE_MAX_NUM_FEATURES=1024`,
`COLMAP_COMPARE_THREADS=6`, sequential overlap `10`, and loop detection
disabled.

These original numbers used COLMAP's normal SIFT cap semantics, not an exact
post-extraction row cap. CPU SIFT can exceed `max_num_features` because it keeps
scale levels and orientation duplicates together, while the imported SiftMetal
path currently emits a hard-capped row count. The `compare:extractor` Mise task
now defaults to exact-capping feature rows after extraction and to one extractor
worker per CPU/Metal path, so future runs are better feature-count and
single-worker timing comparisons. Set `COLMAP_COMPARE_EXACT_FEATURE_CAP=0` to
reproduce nominal COLMAP cap behavior, or set
`COLMAP_COMPARE_CPU_EXTRACT_THREADS` / `COLMAP_COMPARE_METAL_EXTRACT_THREADS`
explicitly for throughput experiments.

Follow-up extraction-only probe on the local 41-frame comparison set
(`data/mise/runs/extract-thread-probe-20260630-133348`) clarified the threading
behavior:

- CPU SIFT with `FeatureExtraction.num_threads=1`: `26.49s` real, `57079`
  nominal feature rows; exact-capping trims this to `41984` rows.
- CPU SIFT with `FeatureExtraction.num_threads=6`: `7.99s` real, same nominal
  and capped row counts.
- SiftMetal with `FeatureExtraction.num_threads=1`: `20.37s` real, `41984`
  rows before and after exact-capping.
- SiftMetal with `FeatureExtraction.num_threads=6`: `5.10s` real, `41984`
  rows before and after exact-capping.

The Metal controller still creates one SiftMetal extractor for the default
single Metal device path; requesting more extraction threads mainly increases
controller-side resizer/feed workers. This supports keeping the single Metal
extractor guard while the imported extractor remains research-grade, and using
the exact-cap harness when comparing reconstruction quality.

Observed runtimes:

- CPU extraction: `58.60s` real.
- SiftMetal extraction: `53.60s` real.
- CPU matcher on CPU descriptors: `73.88s` real.
- CPU matcher on SiftMetal descriptors: `110.57s` real.
- Metal matcher on SiftMetal descriptors: `3.06s` real.

Database summary:

| Run | Images | Keypoints | Raw matches | Verified inliers | Verified pairs |
| --- | ---: | ---: | ---: | ---: | ---: |
| CPU extraction + CPU matching | 100 | 150403 | 70144 | 60917 | 323 |
| SiftMetal extraction + CPU matching | 100 | 102400 | 45844 | 39390 | 308 |
| SiftMetal extraction + Metal matching | 100 | 102400 | 45577 | 39256 | 309 |

Largest sparse model summary:

| Run | Registered images | Points | Observations | Reprojection error |
| --- | ---: | ---: | ---: | ---: |
| CPU extraction + CPU matching | 75 | 6530 | 29637 | `1.223991px` |
| SiftMetal extraction + CPU matching | 68 | 4380 | 18990 | `1.252233px` |
| SiftMetal extraction + Metal matching | 67 | 4338 | 18924 | `1.255061px` |

This is a successful end-to-end sparse reconstruction smoke test for the
experimental extractor, but it is not production parity. The same configured
feature cap produced different keypoint counts across CPU SIFT and SiftMetal,
and the largest SiftMetal reconstruction registered fewer images and points
than the CPU baseline. Treat the extractor as runnable research-grade code
until keypoint limiting, descriptor parity, and reconstruction density improve.

A smaller local video-frame comparison was also run on 2026-06-30 with
`mise run compare:frames:extractor` against 41 JPEG frames from
`splatdata-f5e30b7f/attempts/starter-20260630-124559/frames/all`.

Like the Gerrard Hall run above, these frame numbers were collected before the
exact-cap comparison harness change.

Observed matcher runtimes on that set:

- CPU matcher on CPU descriptors: `20.59s` real.
- CPU matcher on SiftMetal descriptors: `35.24s` real.
- Metal matcher on SiftMetal descriptors: `1.41s` real.

Frames database summary:

| Run | Images | Keypoints | Raw matches | Verified inliers | Verified pairs |
| --- | ---: | ---: | ---: | ---: | ---: |
| CPU extraction + CPU matching | 41 | 57079 | 34773 | 34237 | 106 |
| SiftMetal extraction + CPU matching | 41 | 41984 | 25831 | 25458 | 102 |
| SiftMetal extraction + Metal matching | 41 | 41984 | 25754 | 25377 | 101 |

Frames sparse model summary:

| Run | Registered images | Points | Observations | Reprojection error |
| --- | ---: | ---: | ---: | ---: |
| CPU extraction + CPU matching | 41 | 3862 | 18058 | `0.888671px` |
| SiftMetal extraction + CPU matching | 41 | 2912 | 13480 | `0.889067px` |
| SiftMetal extraction + Metal matching | 41 | 2901 | 13430 | `0.888235px` |

The frames set confirms that the extractor can support complete sparse
registration on local video-derived data, but it repeats the same density gap:
SiftMetal produces fewer keypoints, inliers, points, and observations than the
CPU baseline.

The imported SiftMetal prior art remains in the research-grade bucket until the
following port-specific issues are resolved:

- Validate descriptor ordering, normalization, and quantization against
  COLMAP's CPU SIFT output.
- Allocate pyramid textures from actual resized image dimensions instead of the
  square `max_image_size x max_image_size` upper bound.
- Align keypoint limiting semantics with COLMAP CPU SIFT or document an
  intentional SiftGPU-style behavior difference.
- Report Metal device, pipeline, and shader-library failures with enough detail
  to diagnose user toolchain issues.
- Fix shader compiler warnings before treating the extractor as an upstreamable
  production path.

Resolved in this branch:

- Extrema compaction now passes the candidate buffer capacity into
  `siftExtremaList`, uses a `uint32_t` counter matching Metal's `atomic_uint`,
  and drops excess candidates before writing past the fixed candidate buffer.
- Descriptor generation now returns standard SIFT float descriptors from the
  Metal kernel instead of quantizing to bytes before COLMAP applies its
  configured L2/RootSIFT normalization.

## Integration Risks

- Descriptor parity: Small descriptor-order or normalization differences can
  silently degrade brute-force, FAISS, and LightGlue matching quality.
- Coordinate convention: COLMAP keypoints use pixel-center coordinates; Metal
  kernels naturally index integer pixels, so the `+0.5` convention must stay
  explicit.
- Scale convention: First octave, upsampling, and octave-to-original scale
  mapping must match existing SIFT behavior before reconstruction comparisons
  are meaningful.
- Feature limiting: Changing which keypoints survive `max_num_features` can
  alter downstream match graph density even if descriptors are good.
- Platform gating: Metal requires Apple platforms and Objective-C++ build
  handling; integration should sit behind an Apple/Metal CMake option.
- Runtime overhead: Runtime shader compilation is fine for the spike but should
  become cached/default library loading in production.
- Concurrency: Metal should avoid SiftGPU's global static state issues, but
  production still needs clear per-device queue ownership and extractor thread
  rules. The current integration serializes the experimental Metal extractor to
  one worker while this is being validated.
- Readback cost: Pulling candidates/descriptors to CPU between stages is easy
  but can erase GPU gains. The first production pass should keep limiting and
  descriptor normalization on GPU.

## Suggested Next Steps

The current integration remains opt-in with `-DSIFT_METAL_ENABLED=ON` and
runtime `--FeatureExtraction.use_gpu 1 --SiftExtraction.use_metal 1`. The
Metal path maps the baseline non-covariant SIFT options for octave layout,
thresholds, feature caps, orientation count, and upright mode, then lets COLMAP
apply the requested descriptor normalization. Affine shape, domain-size pooling,
and forced covariant extraction fall back to CPU covariant SIFT; Metal logs that
darkness adaptivity is ignored because it is only implemented by GLSL SiftGPU.
The controller uses one extractor worker on the default Metal device and ignores
`FeatureExtraction.gpu_index` for this backend.

Focused validation without requiring a Metal extraction device:

```bash
mise run test:metal
```

1. Validate descriptor ordering and normalization against VLFeat on synthetic
   images, then decide whether final normalization should move onto the GPU.
2. Add parity tests for keypoint coordinate/scale/orientation ranges and
   descriptor shape/type before measuring reconstruction quality.
3. Benchmark extraction-only runtime and end-to-end matching/reconstruction on
   a small image set before replacing any existing SIFT backend.
