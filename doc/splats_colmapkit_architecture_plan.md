# Splats ColmapKit Architecture Plan

Date: 2026-06-30

## Coordinator Goal

Build the next Splats architecture plan: make COLMAP an Apple-native engine inside Splats, then extend Splats into a capture -> compute -> view pipeline across iPhone, iPad, and macOS.

Use the COLMAP fork at `/Users/brw/Developer/ai-projects/colmap` and the Splats branch `codex/splats-run-cockpit-ipad-20260630` as the source of truth. The current Splats Run Cockpit shells out to an external `colmap` binary on macOS. The new direction is to package the useful COLMAP sparse reconstruction path as an Apple-native library, likely `ColmapKit.xcframework`, and integrate it directly with Splats while preserving the existing shell runner as a fallback.

## Current Evidence

- COLMAP has separate build gates for Apple Metal matching and experimental SiftMetal extraction: `METAL_ENABLED` and `SIFT_METAL_ENABLED`.
- COLMAP already builds position-independent code and has an install/export path that can be shaped into a framework packaging flow.
- The sparse pipeline has callable C++ entry points below the CLI layer: feature extraction controllers, feature matcher controllers, and `RunIncrementalMapperImpl`.
- The Metal matcher is the first production acceleration candidate. SiftMetal extraction has improved in the current notes, but it still needs fixture-level validation before replacing CPU SIFT in Splats.
- The target Splats branch defines `SplatsWorkflow` and `SplatsRunCockpit` as macOS-only targets. `SplatsiOS` currently does not depend on them.
- Splats workflow execution currently depends on `Process`, zsh scripts, `python3`, `ffmpeg`, external `colmap`, external `brush`, and bundled helper scripts.
- The Run Cockpit already expects COLMAP output shape: `database.db`, sparse binary models, sparse text models, component reports, and preview data parsed from `images.txt` and `points3D.txt`.

## Product Vision

Splats becomes the whole local splat pipeline:

1. Capture source data on iPhone, iPad, or macOS.
2. Collect images or video frames plus camera intrinsics, timestamps, GPS, IMU, ARKit pose, and depth when available.
3. Run COLMAP sparse reconstruction and downstream training locally or on a stronger nearby Mac.
4. Publish a finished splat artifact with enough provenance to reopen, compare, or reproduce the run.
5. Make the result viewable from any local Splats instance.

The near-term workflow is:

1. iPhone or iPad captures a splat dataset.
2. Splats transfers the capture bundle to Splats running on macOS over the local network.
3. The Mac performs heavier COLMAP and training work.
4. Progress streams back into the Run Cockpit.
5. The final splat becomes discoverable and viewable on iPad, iPhone, or Mac.

## Architecture Shape

### COLMAP Layer

Create a narrow Apple-facing facade instead of exposing COLMAP C++ to Swift.

- Add a `ColmapKit` C or Objective-C++ facade target inside the COLMAP fork.
- Keep the ABI small and stable: opaque handles, plain structs, callbacks, and explicit error objects.
- First operation: run sparse reconstruction from a structured config.
- Emit progress events for extraction, matching, mapping, model writing, and failure diagnostics.
- Preserve existing COLMAP output folders so Splats can reuse its current parsers and component reports.
- Treat MVS, GUI, pycolmap, ONNX, CGAL, OpenGL/SiftGPU, and CUDA as out of scope for the first package.

### Packaging Layer

Package a minimal build as `ColmapKit.xcframework`.

- Start with macOS arm64.
- Then attempt iOS device and iOS simulator slices as feasibility probes.
- Prefer static internal COLMAP libraries linked into a framework-shaped product unless dependency size or license handling forces a different shape.
- Audit with `otool -L` so the product does not accidentally depend on Homebrew dylibs.
- Include license and third-party notice output as part of the package.
- If SiftMetal extraction ships, place `sift.metallib` in the framework resources and load it from the framework bundle, not only from `Bundle.main`.

### Splats Layer

Add an explicit embedded COLMAP boundary instead of wiring ColmapKit directly into the cockpit UI.

- Work from `codex/splats-run-cockpit-ipad-20260630` in a separate worktree.
- Add a wrapper target, likely `SplatsCOLMAP`.
- Define a runner protocol in Splats that can support:
  - external-process COLMAP on macOS,
  - embedded ColmapKit where available,
  - remote Mac compute over the local network.
- Keep current shell/process execution as a fallback and migration baseline.
- Keep Run Cockpit state reducer-driven. TCA is a good fit for progress streams, cancellation, retries, and replaceable runner dependencies.
- Keep low-level ColmapKit calls in plain infrastructure clients, injected into reducers rather than stored in reducer state.

### Workflow Boundary

Do not make all of `SplatsWorkflow` multiplatform in one jump.

- Extract a smaller shared workflow core if needed: capture bundle models, attempt settings, run request schemas, progress events, component summary models, and preview parsing.
- Leave `Process`, zsh, Finder/Terminal affordances, detached launch, and local executable resolution in macOS-only code.
- Port helper-script behavior that iPad needs into Swift before enabling iPad workflows.
- Keep file formats compatible with the existing cockpit: `run-request.json`, `preflight.json`, logs, reports, sparse text, and component summaries.

## Capture Bundle Contract

Define a portable capture bundle that can be created on iOS/iPadOS and consumed on macOS.

Suggested first layout:

```text
CaptureName.splatscapture/
  capture.json
  images/
    frame_000001.heic
    frame_000002.heic
  video/
    source.mov
  metadata/
    frames.jsonl
    cameras.json
    device.json
    location.jsonl
    motion.jsonl
    arkit-poses.jsonl
    depth/
  checksums.json
```

Required fields:

- stable capture id
- app version and device model
- source asset references
- per-frame timestamp
- per-frame image path
- image dimensions and orientation
- camera intrinsics when available
- EXIF and lens metadata when available

Optional fields:

- GPS and altitude
- IMU samples
- ARKit camera pose
- depth map references
- confidence maps
- user notes or scene labels

The bundle should be appendable during capture and sealed before transfer with checksums and a manifest version.

## Local Network Compute Contract

Model nearby Macs as compute nodes rather than as hidden implementation details.

- Discover local Splats instances with Bonjour/Network.framework.
- Pair devices explicitly before accepting capture transfers.
- Transfer sealed capture bundles over a resumable local protocol.
- Represent compute jobs with ids, input bundle ids, runner kind, status, progress events, logs, cancellation state, and output artifact references.
- Stream job progress back to the originating device.
- Publish finished artifacts through a local catalog so any paired Splats instance can browse and open them.

Minimum job states:

```text
queued -> transferring -> preparing -> extracting -> matching -> mapping -> training -> packaging -> published
```

Terminal states:

```text
succeeded, failed, cancelled, outputMissing
```

## Subagent Charters

### 1. COLMAPKit API Agent

Mission: create the smallest stable native facade over the COLMAP sparse pipeline.

Tasks:

- Design `ColmapKit` public C/Objective-C++ headers.
- Define `ColmapKitSparseReconstructionConfig`.
- Define progress and log callback events.
- Implement a macOS proof path using COLMAP controllers and `RunIncrementalMapperImpl`.
- Preserve output layout compatible with the current Splats cockpit.

Deliverables:

- New COLMAP facade target.
- A tiny command-line or Swift-callable sample that links the facade.
- API notes documenting what is intentionally not exposed.

Validation:

- Run a 5-10 image sparse fixture through CLI COLMAP and ColmapKit.
- Compare database existence, sparse model creation, sparse text conversion, registered image count, sparse point count, and reprojection error range.

### 2. XCFramework Packaging Agent

Mission: make a reproducible Apple framework package for the minimal COLMAP engine.

Tasks:

- Add CMake presets or scripts for minimal Apple builds.
- Build macOS arm64 first.
- Attempt iOS device and simulator slices after macOS links.
- Disable GUI, CUDA, OpenGL/SiftGPU, MVS, ONNX, CGAL, and pycolmap initially.
- Audit dependency closure and dynamic links.
- Package headers, module map, framework binary, resources, and notices.

Deliverables:

- `ColmapKit.xcframework`.
- Build script or CMake preset documentation.
- Dependency and license manifest.
- iOS feasibility report with concrete compiler/linker blockers if any.

Validation:

- `xcodebuild -create-xcframework` succeeds for supported slices.
- `otool -L` shows no accidental Homebrew runtime dylib dependencies.
- A minimal Swift sample imports and calls the package.

### 3. Metal Runtime Agent

Mission: keep Metal acceleration useful but correctly staged.

Tasks:

- Keep `METAL_ENABLED` separate from `SIFT_METAL_ENABLED`.
- Treat Metal descriptor matching as the first production acceleration candidate.
- Re-run SiftMetal extraction parity and speed checks on Splats-like image fixtures.
- If SiftMetal extraction remains enabled, make framework-bundle resource lookup work for `sift.metallib`.
- Document whether the default Splats embedded path should use CPU SIFT plus Metal matching, SiftMetal extraction, or CPU-only.

Deliverables:

- Metal runtime decision memo.
- Resource-loading patch if SiftMetal ships in the framework.
- Fixture benchmark table.

Recommended default for this phase: CPU SIFT + CPU matching as the Splats embedded
baseline; then evaluate `use_metal_matching` as the first acceleration candidate
once the Metal path is proven non-fallback in the runtime packaging environment.
SiftMetal extraction remains experimental and not the default.

Validation:

- COLMAP Metal tests pass.
- Fixture comparisons include feature count, verified pairs, registered images, sparse points, observations, reprojection error, and wall time.

Cross-check the current decision in
[doc/colmapkit_metal_runtime.md](colmapkit_metal_runtime.md).

### 4. Splats Integration Agent

Mission: introduce ColmapKit without breaking current macOS cockpit behavior.

Tasks:

- Work in a separate Splats worktree from `codex/splats-run-cockpit-ipad-20260630`.
- Add `SplatsCOLMAP` or equivalent wrapper target.
- Define a runner protocol for COLMAP solve operations.
- Implement external-process runner as the current baseline.
- Add embedded runner behind availability/build flags.
- Route cockpit actions through the runner protocol.

Deliverables:

- Splats target and dependency wiring.
- Runner protocol and two implementations.
- Tests proving fallback behavior still works.

Validation:

- Tuist generation succeeds.
- Existing macOS Run Cockpit tests pass.
- A macOS app build still supports the current external-process COLMAP flow.

### 5. Workflow/iPad Boundary Agent

Mission: make the shared workflow pieces iPad-ready without dragging macOS process code onto iPad.

Tasks:

- Inventory `SplatsWorkflow` APIs into shared, macOS-only, and candidate-new-target groups.
- Extract or mark a shared workflow core for capture bundles, run requests, progress events, preview parsing, and component readiness.
- Move `Process`, zsh, Finder, Terminal, and executable resolution behind macOS-only boundaries.
- Port component-report and readiness logic from helper scripts into Swift where iPad needs it.

Deliverables:

- Boundary report.
- Target proposal for `SplatsWorkflowCore` or a carefully widened `SplatsWorkflow`.
- iPad build feasibility patch or blocker list.

Validation:

- iOS/iPad target builds with the shared workflow core.
- macOS workflow tests continue to pass.

### 6. Local Network Agent

Mission: design capture transfer and remote Mac compute as first-class Splats workflow primitives.

Tasks:

- Define local discovery and pairing model.
- Define sealed capture bundle transfer protocol.
- Define remote job request, progress event stream, cancellation, and artifact publishing.
- Decide where remote compute state lives in the Run Cockpit reducer.
- Document trust, local network permission, and retry behavior.

Deliverables:

- Network architecture document.
- Swift protocol sketches for discovery, transfer, job control, and artifact catalog.
- A local mock transport for tests and previews.

Validation:

- Unit tests can simulate capture upload, remote progress, cancellation, failure, and artifact publication without a real network.

### 7. Validation Agent

Mission: keep this architecture honest with fixture-level checks.

Tasks:

- Build a tiny fixture pipeline with 5-10 images.
- Compare CLI COLMAP output against embedded ColmapKit output.
- Validate Splats generation/builds after integration.
- Track known blockers by category.

Required checks:

- COLMAP minimal package build.
- Swift sample link against `ColmapKit.xcframework`.
- Splats Tuist generation.
- macOS cockpit tests.
- iOS/iPad build feasibility.
- Fixture sparse reconstruction comparison.

Blocker categories:

- dependency blockers
- platform blockers
- packaging/signing blockers
- product workflow blockers
- performance/quality blockers

## Implementation Sequence

1. Prove macOS `ColmapKit` can run sparse reconstruction without Swift or Splats involved.
2. Package macOS arm64 as an XCFramework slice and link a tiny Swift sample.
3. Add Splats runner protocol and keep the external process runner as default.
4. Add embedded runner behind a feature flag on macOS.
5. Compare embedded output to current CLI output on the same fixture and on one real Splats attempt.
6. Probe iOS device and simulator slices and record real blockers.
7. Extract shared workflow core for iPad capture and remote-job UI.
8. Add capture bundle format and local network compute model.
9. Make remote Mac compute the preferred iPad path while embedded iOS COLMAP remains a measured option.

## Delivery Slices and Commit Plan

This plan should be delivered in coherent parts, not as one all-or-nothing change. Each part should leave the repo in a reviewable state, preserve existing behavior, and record any unproven assumptions before moving to the next slice.

Commit boundaries should follow these rules:

- One commit per durable integration boundary or validation artifact.
- Each commit should build or document why it cannot yet build in the current environment.
- Do not mix COLMAP packaging work, Splats app integration, iPad workflow extraction, and network compute in the same commit.
- Keep fallback behavior working before introducing a replacement path.
- When a slice discovers a blocker, commit the blocker documentation separately from speculative fixes unless the fix is already proven.

Planned commits:

1. `feat(colmapkit): add sparse reconstruction facade`
   - Scope: `src/colmap/colmapkit/**`, CMake target wiring, link-validation sample, facade API notes.
   - Validation: build `colmapkit_sparse_reconstruct`, run sample help/error paths, run `git diff --check`.
   - Exit criteria: native C ABI exists and links without requiring Splats.

2. `build(colmapkit): add macOS framework packaging path`
   - Scope: optional `ColmapKit.framework` target, module map, packaging script, `otool -L` audit output path, packaging docs.
   - Validation: run `bash scripts/build_colmapkit_xcframework.sh`, create a macOS arm64 `ColmapKit.xcframework`, record dynamic dependencies, and prove Swift can import and call `ColmapKitVersion`.
   - Exit criteria: a reproducible macOS packaging command exists, even if dependency blockers remain.

3. `test(colmapkit): add sparse reconstruction fixture comparison`
   - Scope: tiny 5-10 image fixture or fixture-fetch instructions, CLI-vs-ColmapKit comparison script, expected metric tolerances.
   - Validation: compare database creation, sparse model existence, sparse text export, registered images, sparse points, observations, and reprojection error.
   - Exit criteria: ColmapKit output is proven comparable to CLI COLMAP on a small dataset.

4. `docs(colmapkit): document dependency and platform blockers`
   - Scope: blocker matrix for OpenMP, CHOLMOD/SuiteSparse, Homebrew dylibs, static-vs-dynamic linking, iOS simulator/device constraints, Metal resource packaging.
   - Validation: concrete configure/build/link commands and error excerpts for each blocker.
   - Exit criteria: packaging blockers are categorized and actionable.

5. `feat(splats-colmap): add Splats runner boundary`
   - Scope: new Splats-side wrapper target, runner protocol, external-process runner as baseline, tests for fallback behavior.
   - Validation: Tuist generation, macOS cockpit tests, existing external COLMAP flow still available.
   - Exit criteria: Run Cockpit talks to a runner abstraction instead of directly owning one execution mechanism.

6. `feat(splats-colmap): add embedded ColmapKit runner`
   - Scope: Splats embedded runner behind build/availability flags, ColmapKit progress-to-cockpit event mapping, failure propagation.
   - Validation: macOS build links ColmapKit, fixture run can use embedded path, external runner remains selectable.
   - Exit criteria: embedded COLMAP is integrated without removing current macOS shell fallback.

7. `refactor(splats-workflow): extract iPad-safe workflow core`
   - Scope: shared workflow models for capture bundles, run requests, progress events, preview parsing, component readiness; macOS-only process code isolated.
   - Validation: iOS/iPad target builds with shared core; macOS workflow tests still pass.
   - Exit criteria: iPad can understand workflow/capture/remote-job state without importing `Process` or shell execution code.

8. `feat(splats-capture): define capture bundle format`
   - Scope: `.splatscapture` schema, manifest/checksum model, metadata model for camera intrinsics, timestamps, GPS, IMU, ARKit pose, depth references.
   - Validation: encode/decode tests, append-and-seal behavior, backwards-compatible manifest versioning.
   - Exit criteria: iPhone/iPad/macOS can produce or consume the same capture bundle contract.

9. `feat(splats-network): add local compute protocol skeleton`
   - Scope: discovery, pairing model, transfer protocol interfaces, remote job model, progress stream model, artifact catalog protocol, mock transport.
   - Validation: unit tests simulate upload, progress, cancellation, failure, publish, and artifact open without real network.
   - Exit criteria: Run Cockpit can model remote Mac compute as a first-class runner path.

10. `feat(splats-network): connect capture to remote Mac compute`
    - Scope: real local network transport, Mac compute-node receiver, capture upload, job execution handoff, result publication.
    - Validation: one local iPad/iPhone-to-Mac capture transfer and remote job smoke test, final artifact discoverable by another Splats instance.
    - Exit criteria: capture here, compute on Mac, view anywhere works as an end-to-end product slice.

11. `build(colmapkit): probe iOS xcframework slices`
    - Scope: iOS device and simulator configure/build attempts, iOS-specific dependency audit, linker error documentation or successful slices.
    - Validation: `xcodebuild -create-xcframework` includes any proven iOS slices, or blocker docs explain exactly why not.
    - Exit criteria: iPad feasibility is tested with evidence, not assumed.

The order can change when a later slice is lower risk or unlocks useful evidence sooner, but commit boundaries should stay intact. For example, dependency-blocker documentation may land before fixture work if packaging fails early, and Splats runner protocol work can begin while ColmapKit packaging is still being hardened.

## Acceptance Criteria

- A clear `ColmapKit.xcframework` packaging path exists.
- Splats has an explicit embedded COLMAP integration boundary.
- Existing macOS Run Cockpit behavior remains available.
- iPad feasibility is tested, not assumed.
- The capture -> remote Mac compute -> view anywhere architecture is documented.
- Known blockers are separated into dependency, platform, packaging, and product workflow categories.
- Subagents have concrete deliverables and validation checks.

## Implementation Progress

### 2026-06-30: Initial COLMAPKit Facade

Implemented:

- Added `src/colmap/colmapkit/colmapkit.h` with a narrow C ABI for sparse reconstruction.
- Added `src/colmap/colmapkit/colmapkit.cc` using COLMAP feature extraction, matching, and `RunIncrementalMapperImpl`.
- Added the `colmap_colmapkit` library target.
- Added `colmapkit_sparse_reconstruct` as a tiny link-validation sample executable.
- Wired `colmap_colmapkit` into the normal COLMAP CMake target graph and install/export list.
- Added `doc/colmapkit_facade.md` with API notes and current validation evidence.

Validated:

- `cmake --build build-codex-metal --target colmapkit_sparse_reconstruct`
- `build-codex-metal/src/colmap/colmapkit/colmapkit_sparse_reconstruct --help`
- `git diff --check`

Known remaining blockers:

- A fresh reduced configure with `OPENMP_ENABLED=OFF` is still blocked by Homebrew CHOLMOD requiring OpenMP through its own CMake package config.
- Splats-side `SplatsCOLMAP` and runner protocol work has not started.
- iOS device/simulator feasibility is blocked by unavailable iOS-compatible third-party dependencies.

### 2026-06-30: macOS Framework Packaging Proof

Implemented:

- Added an optional Apple `ColmapKit.framework` target with public header and module map packaging.
- Added `scripts/build_colmapkit_xcframework.sh` for a reproducible macOS arm64 package build.
- Made the package script use `/usr/bin/cc`, `/usr/bin/c++`, Homebrew `libomp` hints, and `CMAKE_IGNORE_PREFIX_PATH=/opt/anaconda3` to avoid Conda package leakage.
- Generated `dist/colmapkit/ColmapKit.xcframework` and `dist/colmapkit/ColmapKit-otool-L.txt`.

Validated:

- `bash scripts/build_colmapkit_xcframework.sh`
- `xcodebuild -create-xcframework` through the packaging script
- `otool -L` audit written to `dist/colmapkit/ColmapKit-otool-L.txt`
- `swift -module-cache-path /private/tmp/colmapkit-swift-module-cache -F dist/colmapkit/ColmapKit.xcframework/macos-arm64 -framework ColmapKit -e 'import ColmapKit; print(String(cString: ColmapKitVersion()))'`

Known remaining blockers:

- The local package is not signed: `codesign --verify --deep --strict --verbose=2 dist/colmapkit/ColmapKit.xcframework` reports `code object is not signed at all`.
- `otool -L` still shows Homebrew runtime dylibs for Boost, Ceres, OpenImageIO, glog, gflags, Metis, libomp, and SuiteSparse/CHOLMOD.
- The link emits deployment-target warnings because the current Homebrew dylibs were built for macOS 26.0 while the package script targets macOS 13.0.
- iOS device/simulator slices are still untested.

### 2026-06-30: Sparse Fixture Comparison Harness

Implemented:

- Added `scripts/python/colmapkit_compare.py` to run CLI COLMAP and ColmapKit against the same small image set.
- Added a deterministic synthetic 8-image fixture generator inside the comparison script, avoiding checked-in binary test photos.
- Added `mapper_random_seed` to `ColmapKitSparseReconstructionConfig` and the `colmapkit_sparse_reconstruct` sample so CLI and ColmapKit mapper runs can be seeded the same way.
- Aligned ColmapKit CPU matching with CLI COLMAP by preserving the default FAISS-backed CPU matcher when Metal matching is disabled.

Validated:

- `cmake --build build-codex-metal --target colmapkit_sparse_reconstruct`
- `python3 -m py_compile scripts/python/colmapkit_compare.py`
- `python3 scripts/python/colmapkit_compare.py --colmap-bin build-codex-metal/src/colmap/exe/colmap --colmapkit-bin build-codex-metal/src/colmap/colmapkit/colmapkit_sparse_reconstruct --run-dir /private/tmp/colmapkit-compare-run --generate-synthetic-fixture --force`

Comparison result:

- database exists for both paths
- images: 8
- keypoints/descriptors: 54130
- verified pairs: 17
- verified inliers: 3615
- sparse models: 1
- registered images: 8
- sparse points: 612
- observations: 2453
- mean reprojection error: 0.3380671744135806

Known remaining blockers:

- The fixture is synthetic. A real Splats-like photo/video-frame capture still needs to be run through CLI COLMAP and ColmapKit.
- Metal matching and SiftMetal extraction are not covered by this CPU fixture comparison.

### 2026-06-30: iOS Slice Feasibility Probe

Implemented:

- Added `scripts/probe_colmapkit_ios.sh` to configure iOS device and iOS simulator ColmapKit slices.
- Made the probe use Xcode SDK `clang`/`clang++` instead of ambient compiler wrappers.
- Made strict dependency mode the default so the probe ignores macOS host package prefixes and avoids false-positive links against Homebrew or Conda libraries.
- Wrote probe logs and a Markdown summary under `dist/colmapkit-ios-probe/`.
- Added `doc/colmapkit_packaging_blockers.md` as the shared blocker matrix for dependency, platform, packaging, signing, and Metal runtime follow-up.

Validated:

- `bash -n scripts/probe_colmapkit_ios.sh`
- `bash scripts/probe_colmapkit_ios.sh`

Probe result:

- iOS device slice configure status: failed.
- iOS simulator arm64 slice configure status: failed.
- First concrete blocker for both slices: no iOS-compatible Boost CMake package is available to the strict probe.
- Representative CMake error: `Could not find a package configuration file provided by "Boost"`.

Known remaining blockers:

- The probe stops at Boost, so the rest of the iOS dependency closure remains unproven: Eigen, OpenImageIO, Metis, glog, SQLite, CHOLMOD/SuiteSparse, Ceres, PoseLib, FAISS, and Metal resource behavior.
- A real iOS dependency prefix/toolchain is needed before `ColmapKit.xcframework` can include device or simulator slices.
- Host macOS Homebrew dylibs are not acceptable evidence for iOS feasibility.
