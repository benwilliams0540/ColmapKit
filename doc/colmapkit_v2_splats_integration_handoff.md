# ColmapKit ABI V2 -> Splats integration handoff

Status: local implementation handoff; not a release or publication instruction.

## Authorities and non-goals

- ColmapKit source base: `4b846a69b517a5292691033e735a62863fc25e00`
  (`colmapkit-v0.2.1`), plus the local additive V2 work on
  `perf/tracked-pose-rgb-prior-abi`.
- Splats consumer authority: exactly
  `8b3145bcb20a1de3b31ee1b9d0caf3c044241d71`. Read files with
  `git show 8b3145bcb20a1de3b31ee1b9d0caf3c044241d71:<path>`; do not substitute
  a newer working-tree snapshot.
- The existing ColmapKit v0.2.1 entry points and behavior remain available.
- This handoff does not authorize a push, PR, CI run, tag, release, or edits to
  BrushKit. Physical execution on `iPad (2)` is a later coordinated gate.
- No fallback from benchmark C or D to the legacy reconstruction route is
  permitted. A V2 failure must make that benchmark attempt fail explicitly.

## Local binary to integrate

The completed build writes these values to
`dist/colmapkit-v2-local/artifact-summary.txt`:

```text
xcframework=/Users/brw/Developer/ai-projects/colmap/dist/colmapkit-v2-local/ColmapKit.xcframework
zip=/Users/brw/Developer/ai-projects/colmap/dist/colmapkit-v2-local/ColmapKit.xcframework.zip
zip_sha256=<copy the exact value from artifact-summary.txt>
swiftpm_checksum=<copy the exact value from artifact-summary.txt>
slices=macos-arm64,ios-arm64,ios-arm64-simulator
```

Do not copy checksum values from this handoff or a prior build: the generated
summary is the single exact authority for the final local artifact.

For a local integration pass, copy the XCFramework into a consumer-owned local
artifact location and change `Vendor/ColmapKit/Package.swift` from the released
URL target to a local `.binaryTarget(name:path:)`. Do not change the published
v0.2.1 URL/checksum as though this local build were a release. Record both the
ColmapKit source SHA and the actual XCFramework zip SHA-256 in Splats benchmark
environment evidence.

At process startup, require all three checks:

```swift
precondition(ColmapKitGetABIVersionV2() == 2)
precondition(!String(cString: ColmapKitGetReleaseVersionV2()).isEmpty)
precondition(!String(cString: ColmapKitGetEngineBuildIdentityV2()).isEmpty)
```

`ColmapKitVersion()` remains the legacy underlying COLMAP engine version. Do
not use it as the V2 facade release or ABI identity.

## Capture-manifest mapping

At the authority SHA, `SplatComputeCaptureManifest.currentSchemaVersion` is 4.
For benchmark C/D, build the ABI array from the already-frozen selected training
frame order, after held-out removal. Before allocating any ABI values, reject a
frame unless all of these are true:

1. `trackingEvidence.state == .normal`.
2. `poseEvidence` exists, contains 16 finite values, declares
   `.columnMajor` and `.arkitWorldFromCamera`, and is temporally acceptable
   under the existing capture evidence contract.
3. If `cameraTransform` is also present, it is identical to
   `poseEvidence.transform`; disagreement is an input error.
4. The encoded image exists and its decoded dimensions equal
   `intrinsics.imageWidth` and `intrinsics.imageHeight` after the manifest's
   encoded-image rotation has already been applied.

Limited and unavailable frames are excluded before calling ColmapKit. Do not
pass them with heuristic weights. The benchmark's accepted normal frames use
exact translation and rotation weights of `1.0`.

Map every accepted frame to `ColmapKitTrackedImageV2` as follows:

| V2 field | Splats source/value |
| --- | --- |
| `struct_size` | `UInt32(MemoryLayout<ColmapKitTrackedImageV2>.size)` |
| `camera_model` | `COLMAPKIT_CAMERA_MODEL_V2_PINHOLE.rawValue` |
| `stable_id` | stable UInt64 assigned before filtering and retained in the frozen selection |
| `order_index` | zero-based index in the frozen selected-training-frame order |
| `encoded_width`, `encoded_height` | per-frame `intrinsics.imageWidth`, `imageHeight` |
| `num_camera_params` | `4` |
| `camera_params[0...3]` | `focalLengthX`, `focalLengthY`, `principalPointX`, `principalPointY` |
| `world_from_camera[0...15]` | `poseEvidence.transform`, unchanged, column-major, meters |
| `tracking_state` | `COLMAPKIT_TRACKING_STATE_V2_NORMAL.rawValue` |
| `tracking_reason` | `0` for these accepted normal frames |
| `inclusion_flags` | `COLMAPKIT_TRACKED_IMAGE_FLAG_V2_INCLUDED` |
| `translation_weight`, `rotation_weight` | exactly `1.0`, independently extensible later |
| `image_path` | absolute path to the matching encoded RGB image |

Do not average intrinsics across frames. Keep all C strings and the image array
alive until a synchronous call returns or an async start function has returned;
the async job deep-copies them at start.

The authority SHA still describes pose evidence as comparison-only. The Splats
integration change must version and explicitly promote its use for benchmark
tracked-pose initialization; it must not silently reinterpret old captures.
Schema-4 captures that do not carry the promoted marker are not C/D inputs.

## Exact C and D routes

Both routes first call tracked-pose V2. Use attempt-local, mutually distinct
paths that cannot alias any input RGB file:

```text
<attempt>/colmapkit-v2/database.db
<attempt>/colmapkit-v2/sparse/
<attempt>/colmapkit-v2/refined-poses.json
<attempt>/colmapkit-v2/tracked-evidence.json
```

Freeze this initial benchmark configuration as one named/config-hashed policy:

```text
max_features_per_image=4096
temporal_neighbor_count=3
max_revisit_neighbors_per_image=2
max_image_pairs=200
max_triangulation_passes=2
max_bundle_adjustment_iterations=40
random_seed=<benchmark seed reduced exactly to UInt32>
num_threads=1
revisit_min_translation_meters=0.1
revisit_max_translation_meters=2.0
revisit_max_rotation_degrees=45.0
min_triangulation_angle_degrees=0.1
max_reprojection_error_pixels=4.0
max_allowed_scale_drift_ratio=1e-9
```

These are bounded integration defaults, not representative-capture quality
claims. Persist them and any later change as benchmark configuration evidence.

On success, require:

- wait status and result status are `COLMAPKIT_STATUS_OK`;
- `registered_images == accepted normal frame count`;
- `sparse_points > 0`;
- `measured_scale_drift_ratio <= max_allowed_scale_drift_ratio`;
- `refined_pose_sha256` is exactly 64 lowercase hexadecimal characters;
- tracked evidence reports route `tracked_pose_bounded_v2`,
  `fallback_used=false`, `arkit_world_frame_preserved=true`, and
  `arkit_metric_scale_preserved=true`.

The returned corrections are meters and degrees. Preserve them, the five stage
durations, counts, pose checksum, release version, engine identity, source SHA,
and artifact checksum in Splats reconstruction evidence.

Variant C stops after this tracked reconstruction and uses the resulting sparse
model through the existing sparse-initialization boundary. Its pose source is
the canonical `refined-poses.json`, not the original ARKit values and not a
second unconstrained solve.

Variant D then calls RGB-prior V2 with the same accepted image array, database,
sparse model, pose file, and the exact returned pose SHA. Write:

```text
<attempt>/colmapkit-v2/init.ply
<attempt>/colmapkit-v2/prior-evidence.json
```

Freeze this initial D policy with the benchmark configuration:

```text
normal_neighbor_count=12
max_points_per_spatial_cell=96
max_output_gaussians=500000
minimum_densification_percent=10
random_seed=<same exact UInt32 benchmark seed>
spatial_cell_size_meters=0.08
min_spacing_meters=0.001
max_spacing_meters=0.25
tangent_scale_multiplier=0.8
normal_scale_multiplier=0.2
initial_opacity=0.1
```

Accept D only when both statuses are OK, `variant == 4`, `sh_degree == 0`,
`output_gaussians > sparse_input_points`, the configured densification gate is
met, input/output pose SHA values both equal the tracked result SHA, and the
pose file's bytes are unchanged. Evidence must state `variant=D`,
`poses_frozen=true`, `depth_used=false`, `plane_sweep_used=false`, and
`dense_mvs_used=false`. Otherwise fail D; do not relabel a sparse or point-only
result as the completed D route.

## `init.ply` contract for BrushKit V2

The PLY is binary little-endian 1.0 and declares `comment sh_degree 0`. It has
exactly these Float32 properties in order:

```text
x y z scale_0 scale_1 scale_2 opacity
rot_0 rot_1 rot_2 rot_3 f_dc_0 f_dc_1 f_dc_2
```

- position is in the preserved ARKit world coordinate frame, in meters;
- scales are natural logarithms of positive axis scales;
- scale axes 0 and 1 are tangential and larger than normal axis 2;
- opacity is a logit;
- rotation is a unit quaternion in `w, x, y, z` order;
- `f_dc_0...2 = (rgb / 255 - 0.5) / 0.28209479177387814`;
- no `f_rest_*` fields are present and no higher-order coefficients are guessed.

BrushKit V2 owns zero-padding absent higher bands to its configured training SH
degree. ColmapKit's evidence declares SH degree 0 and source RGB/DC only.

## Job, cancellation, and failure ownership

Use `Start`/`Wait` for app integration. Store the opaque job by Splats job ID,
forward cancellation with the matching V2 cancel function, wait for terminal
status, and call the matching release function exactly once. Repeated wait is
supported, but a released job must never be reused. Progress callbacks arrive
from worker threads; bridge them through a retained `@unchecked Sendable` box
and hop to the appropriate actor before touching UI state.

Treat `INVALID_ARGUMENT`, `RUNTIME_ERROR`, and `CANCELLED` as authoritative.
Persist the result message and available evidence, clean only the attempt-owned
partial output directory, and never invoke legacy reconstruction as a hidden
fallback for C or D.

## Integration acceptance checklist

- Compile and link the local binary from Swift for macOS, iOS device, and iOS
  Simulator; assert ABI 2 and callable legacy plus V2 exports.
- Add adapter tests for schema/promotion, normal-only filtering, exact per-frame
  intrinsics, matrix layout, weights `1.0`, stable ordering, and path ownership.
- Add negative tests for limited/unavailable tracking, missing promoted pose
  evidence, non-rigid/non-finite transforms, mismatched duplicate transforms,
  bad result `struct_size`, pose checksum mismatch, and RGB manifest mismatch.
- Run C and D twice on a committed deterministic fixture and compare canonical
  sparse files, refined poses, and D PLY byte-for-byte.
- Validate every D PLY field and evidence assertion above before BrushKit sees
  it; then prove BrushKit retains DC and zero-pads missing higher bands.
- Run the integrated app in Simulator. This is implementation evidence, not
  physical-device acceptance.
- Before release-ready acceptance, run the coordinated integration on physical
  `iPad (2)`. That gate is deliberately deferred and must remain reported as
  outstanding until performed.
