# Goal Prompt: Upstreamable COLMAP Metal SIFT Integration

/goal Integrate the useful Metal SIFT contributions from
https://github.com/byplay-io/colmap-metal into this COLMAP worktree while
preserving the staged matcher-first Metal plan already underway.

Use Mise as the shared project entrypoint. Before making build or validation
claims, run:

```bash
mise trust
mise run doctor
mise tasks ls
```

If macOS native dependencies are missing, use:

```bash
mise run deps:macos
```

Use these task names in agent reports so results are comparable:

```bash
mise run prior-art:byplay
mise run configure:cpu
mise run configure:metal
mise run configure:sift-metal
mise run build:cpu
mise run build:metal
mise run build:sift-metal
mise run test:metal
mise run install:metal
mise run smoke:metal-extractor
mise run data:gerrard
mise run compare:gerrard
mise run compare:gerrard-extractor
```

Keep CMake, vcpkg, Homebrew, Python, and CI as their native sources of truth.
Mise is the orchestration layer and developer command surface, not a replacement
for those project systems.

Treat the work as two separate Apple-only tracks:

- `METAL_ENABLED`: the production-quality matcher milestone. It must build
  without SiftMetal shader tooling, run focused tests, preserve non-Apple
  behavior, and provide a CPU fallback if Metal is unavailable at runtime.
- `SIFT_METAL_ENABLED`: the experimental SiftMetal extractor track. It may
  import byplay/SIFTMetal prior art behind explicit default-off gates, but it
  must not block the matcher PR. Document any parity, packaging, memory, or
  reconstruction-quality gaps before calling it production-ready.

When running inside Codex or CI, Metal device access may be unavailable even on
Apple Silicon. Agents must distinguish "built with Metal support" from "ran on
hardware Metal" in all reports. Use unsandboxed/local runs for hardware timing
claims.

## Agent Split

Use 5 subagents plus the main orchestrator. This is the ideal split for the next
phase: it gives the matcher, extractor, build system, prior-art review, and
validation independent owners without over-fragmenting shared CMake and option
surfaces.

1. Prior-Art and License Audit Agent
   - Fetch the byplay fork with `mise run prior-art:byplay`.
   - Map the exact Metal extractor files, CMake changes, option changes,
     runtime behavior, shader build flow, installed artifact lookup, and third
     party license obligations.
   - Identify which pieces are upstreamable as-is, which need narrowing, and
     which should stay out of the first PR.

2. Metal Extractor Port Agent
   - Port the extractor contribution behind the default-off Apple-only
     `SIFT_METAL_ENABLED` CMake option, reusing COLMAP's existing feature
     extraction abstractions.
   - Keep this track research-grade until descriptor parity, keypoint
     coordinate conventions, GPU memory bounds, installed `sift.metallib`
     lookup, and single-device/thread behavior are validated.
   - Preserve CPU, CUDA, OpenGL/SiftGPU, ONNX, FAISS, and existing SIFT behavior.
   - Keep Objective-C++ and Metal interop private to implementation files.
   - Prefer byplay's working extraction path as prior art, but fix API,
     packaging, determinism, and documentation issues needed for upstream.

3. Build, Packaging, and Mise Agent
   - Own `.mise.toml`, CMake option naming, shader compilation, `.metallib`
     generation, install/export behavior, and macOS dependency ergonomics.
   - Verify both `mise run configure:cpu && mise run build:cpu` and
     `mise run configure:metal && mise run build:metal`.
   - Verify `mise run configure:sift-metal && mise run build:sift-metal` when
     the local Xcode/Metal toolchain includes `metal` and either `metallib` or
     Metal-driver library linking.
   - Verify `mise run install:metal` and `mise run smoke:metal-extractor`
     when local Metal hardware is available.
   - Ensure Metal-disabled builds do not require Objective-C++, Metal
     frameworks, shader compilers, or byplay/SIFTMetal sources.
   - Ensure SiftMetal-disabled Metal matcher builds do not require shader
     compilers, `sift.metallib`, or byplay/SIFTMetal sources.
   - Keep any new Mise tasks thin wrappers around existing project tooling.

4. Matcher Hardening Agent
   - Harden the existing Metal brute-force SIFT matcher and tests.
   - Confirm top-2 distance behavior, Lowe ratio handling, distance threshold,
     optional mutual/cross-check filtering, deterministic ordering, CPU fallback,
     and integration through COLMAP matching options.
   - Run `mise run test:metal` when local dependencies allow it; otherwise
     document the exact dependency blocker.

5. Validation and Benchmark Agent
   - Use `mise run data:gerrard` and `mise run compare:gerrard` for the longer
     matcher-only dataset comparison.
   - Use `mise run compare:gerrard-extractor` for CPU extraction, Metal
     extraction, and full Metal extraction+matching comparison.
   - Also validate on the local frames dataset when available through
     `COLMAP_FRAMES_DIR`.
   - For extractor parity runs, keep `COLMAP_COMPARE_EXACT_FEATURE_CAP=1`
     unless intentionally measuring COLMAP's native cap behavior. Report
     `COLMAP_COMPARE_CPU_EXTRACT_THREADS`,
     `COLMAP_COMPARE_METAL_EXTRACT_THREADS`, and `COLMAP_COMPARE_MAPPER_THREADS`
     separately; do not compare multi-worker CPU extraction against
     single-worker Metal extraction without labeling it as a throughput run.
   - Report extraction time, matching time, raw match counts, verified inliers,
     registered image counts, sparse point counts, observations, reprojection
     error, and any material model differences.
   - Mark a run as `hardware Metal` only when logs show no CPU fallback warning
     and the process was able to create a real `MTLDevice`.
   - Compare Metal extractor plus Metal matcher against CPU baseline and, where
     available, existing OpenGL/SiftGPU behavior.

## Main Orchestrator

Coordinate the agents, keep edits narrow, and split the final work into
reviewable commits or PRs. Do not import the byplay fork wholesale. Use it as
prior art and port only the smallest production-quality pieces needed for the
next upstreamable milestone.

Recommended PR split:

1. Mise developer workflow and Metal validation harness.
2. Metal matcher hardening and focused tests.
3. SiftMetal prior-art import, build/package scaffolding, and license
   attribution behind `SIFT_METAL_ENABLED`.
4. Experimental Metal extractor integration behind default-off runtime options.
5. Validation docs with matcher benchmark and reconstruction quality results.
6. Follow-up extractor parity/performance PR once the research-grade gaps are
   closed.

## Acceptance Criteria

- `mise run doctor` explains the local toolchain state clearly.
- `mise run configure:cpu && mise run build:cpu` succeeds on macOS.
- `mise run configure:metal && mise run build:metal` succeeds on Apple Silicon
  without requiring SiftMetal shader tooling.
- `mise run configure:sift-metal && mise run build:sift-metal` succeeds on
  Apple Silicon when the selected Xcode/Metal toolchain provides `metal` and
  either `metallib` or Metal-driver library linking.
- Metal-disabled builds preserve existing CPU, CUDA, OpenGL/SiftGPU, ONNX, and
  FAISS paths.
- Metal matcher correctness tests pass or have a documented external dependency
  blocker.
- The Metal extractor contribution is behind explicit Apple-only build and
  runtime options.
- Shader and `.metallib` artifacts work from build trees and installed layouts.
- `mise run smoke:metal-extractor` passes or has a documented external Metal
  hardware/access blocker.
- Gerrard Hall validation shows reconstruction quality close enough to CPU
  baseline to run sparse reconstruction successfully.
- Extractor comparisons are reported separately from matcher comparisons and do
  not gate the matcher deliverable unless explicitly promoted to production.
- Behavior differences from CPU/CUDA/OpenGL paths are documented.
