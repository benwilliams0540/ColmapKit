# ColmapKit upstream integration handoff

Date: 2026-08-18

## Source topology and publication state

- Development base before preservation: `32d15c99a0778cc7f1ee94f74cf07338e4b5bf68`
- Published v0.2.1 source ancestor: `4b846a69b517a5292691033e735a62863fc25e00`
- Preserved pose-free acceleration patch: `d0620d10154416390bd7fd39bc10478fa67966ed`
- Original 13-file dirty-patch SHA-256: `0bfe347150b6d75360981b8f0a25b170fed455f80c06bd439e2060afe8025cfd`
- Pinned and merged upstream tip: `d2da19444e7c9f73ae4a5926c2d6710d04b91f03`
- Explicit merge commit and artifact source: `fd0d8637a822672337e0ea51bd376e5ca80746a7`
- Simulator harness compatibility fix: `c48bd0290ec6f1c6e0c2c79760330d9d6b923796`
- Integration branch: `integrate/upstream-main-2026-08`
- Final upstream refresh: `upstream/main` was still exactly `d2da19444e7c9f73ae4a5926c2d6710d04b91f03`.
- Publication state: local only. Nothing was pushed, tagged, released, uploaded, submitted to CI, or installed on a physical device.

The preservation commit changes these 13 files:

```text
scripts/build_colmapkit_apple_xcframework.sh
scripts/build_colmapkit_xcframework.sh
scripts/probe_colmapkit_ios.sh
src/colmap/colmapkit/CMakeLists.txt
src/colmap/colmapkit/colmapkit.cc
src/colmap/colmapkit/colmapkit.h
src/colmap/colmapkit/sparse_reconstruct.cc
src/colmap/colmapkit/sparse_reconstruction_progress_test.cc
src/colmap/feature/metal_matcher.cc
src/colmap/feature/metal_matcher.h
src/colmap/feature/metal_matcher.mm
src/colmap/feature/sift.cc
src/colmap/feature/sift.h
```

The upstream merge changes 469 files (33,425 insertions and 4,851 deletions). The exact mechanically readable list is:

```bash
git diff --name-status 32d15c99a0778cc7f1ee94f74cf07338e4b5bf68..c48bd0290ec6f1c6e0c2c79760330d9d6b923796
```

## Conflict and semantic-resolution ledger

The only textual merge conflict was the root `CMakeLists.txt`. It was resolved additively:

- retained ColmapKit's `METAL_ENABLED` and `SIFT_METAL_ENABLED` options;
- retained upstream HIP/ROCm options and upstream CUDA-or-HIP selection logic;
- retained upstream OpenMP feature-matching changes, spatial-matching fixes, camera and pose-prior fixes, analytical camera Jacobians and BA changes, and the current PoseLib solver set;
- retained the local incremental sparse-reconstruction route; upstream multi-component global-mapper support does not replace it;
- retained all legacy, post-processing, tracked-pose V2, bounded RGB-prior, cancellation, progress, deterministic-manifest, and Apple-packaging source paths;
- retained the released `num_threads` interpretation and added only struct-size-guarded per-stage overrides;
- retained strict backend reporting: a requested strict Metal route must prove actual dispatch with zero fallback or fail explicitly;
- kept LoMa optional and out of the Apple package because `ONNX_ENABLED=OFF`.

Apple package flags are `METAL_ENABLED=ON`, `SIFT_METAL_ENABLED=OFF`, `ONNX_ENABLED=OFF`, `OPENMP_ENABLED=ON`, `CUDA_ENABLED=OFF`, `GUI_ENABLED=OFF`, and `TESTS_ENABLED=OFF`. Metal descriptor matching remains available to explicit callers but default-off. Metal SIFT is deliberately not claimed by this artifact because the headless test environment could not prove an available Metal device and real dispatch.

## ABI and compatibility proof

All three slices expose identical headers and module maps and export the same 23 ColmapKit symbols. The complete released v0.2.1 export set remains present:

```text
ColmapKitVersion
ColmapKitInitialize
ColmapKitRunSparseReconstruction
ColmapKitStartSparseReconstruction
ColmapKitCancelSparseReconstruction
ColmapKitWaitSparseReconstruction
ColmapKitReleaseSparseReconstructionJob
ColmapKitRunPointFiltering
ColmapKitRunModelCropping
ColmapKitRunModelConversion
```

A client compiled from the published v0.2.1 header against the merged macOS framework reported:

```text
version=COLMAP 4.2.0.dev0 (Commit fd0d8637 on 2026-08-18 without GPU support) config_size=136 result_size=1088 status=1 guard=unchanged
```

This proves the released 136-byte config and 1,088-byte result layout, compile/link/runtime compatibility, expected invalid-input behavior, and bounded writes past a legacy-sized result. The current extended sizes remain 168/1,376 bytes. The existing focused tests cover async input copying, callback ordering and lifetime, cancellation, post-processing, progress, and struct-size guards.

Exact old-header probe commands:

```bash
xcrun clang -x c - -std=c11 \
  -isystem dist/colmapkit-progress-v0.2.1/ColmapKit.xcframework/macos-arm64/ColmapKit.framework/Versions/A/Headers \
  -F dist/colmapkit-upstream-integration-local/ColmapKit.xcframework/macos-arm64 \
  -framework ColmapKit \
  -Wl,-rpath,/Users/brw/Developer/ai-projects/colmap/dist/colmapkit-upstream-integration-local/ColmapKit.xcframework/macos-arm64 \
  -o /private/tmp/colmapkit_old_header_probe < /private/tmp/colmapkit_old_header_probe.c
/private/tmp/colmapkit_old_header_probe
```

## Tests and deterministic behavior

- Full exact-source C++ suite: 149/149 passed in 26.40 seconds.
- Focused ColmapKit suite: 3/3 passed (`model_postprocessing_test`, `sparse_reconstruction_progress_test`, `colmapkit_v2_test`).
- Repeated merged tracked-pose/RGB-prior fixture runs were byte-identical: 8 registered images, 636 sparse points, 3,755 Gaussians, pose SHA-256 `39b67438448a0aab2a95f2fbffc40b18303b038a12ed7b970b642189b4063f43`, PLY SHA-256 `3d04d3dced509697257c4ee9cb3434c1b243113888228514babcf358a4bdf00a`.
- The pre-merge C7 artifact produced 632 sparse points and 3,760 Gaussians on the same fixture. Cameras, images, 54,130 keypoints/descriptors, and 5,836 raw matches are identical. The first logical difference is geometric verification in `two_view_geometries`: 3,948 verified matches before versus 3,938 after. This is attributed to the merged upstream PoseLib/two-view geometry changes; expected outputs were not silently rewritten.

Focused command:

```bash
ctest --test-dir build-colmapkit-v2-dev -R colmapkit --output-on-failure
```

Strict backend evidence was also exercised directly:

- `/private/tmp/colmapkit-strict-sift-fd0d8637.json`: Metal SIFT compiled, no headless default device available, zero dispatches, one recorded fallback attempt, explicit strict failure in 0.0146 seconds, and no CPU continuation.
- `/private/tmp/colmapkit-strict-matching-fd0d8637.json`: Metal matching compiled, no headless device available, zero dispatches/fallbacks, explicit strict failure in 0.0112 seconds, and no CPU continuation.

## Pose-free B4 control and worker semantics

Fixture identity: 115 JPEGs with selected-image manifest SHA-256 `4433a3612c7c2e28c8a86e49c97e80178a586eac08ff1fe5ee0aa7a668aedf12`; `SIMPLE_PINHOLE`, sequential overlap 4, 1024-pixel maximum edge, no pose/GPS/orientation/focus priors.

| Metric | Pre-merge B4 | Merged B4 | Merged W1 |
|---|---:|---:|---:|
| Total seconds | 127.053 | 120.670 | 306.722 |
| Extraction | 58.962 | 58.631 | 217.978 |
| Matching | 12.680 | 14.114 | 36.398 |
| Mapping plus BA | 54.479 | 46.811 | 51.288 |
| Export | 0.824 | 1.046 | 0.978 |
| Peak resident bytes | 1,459,388,416 | 1,593,835,520 | 788,365,312 |
| Registered images | 115 | 115 | 115 |
| Components | 1 | 1 | 1 |
| Sparse points | 46,141 | 46,031 | 45,589 |
| Observations | 178,156 | 177,807 | 176,052 |
| Mean reprojection error | 1.20927 | 1.20888 | 1.20252 |

The one-shot merged B4 result is 5.02% faster than the pre-merge B4 run with all 115 images registered; it is not a repeated-run performance claim. Feature and raw-match counts are identical at 783,861 and 346,487. Verified matches change from 329,307 to 328,982, consistent with the attributed geometric-verification change. The W1 evidence reports effective extraction, matching, geometric verification, mapper, and BA thread counts of one and confirms that the released four-worker policy still performs materially more work in parallel. All three runs used CPU paths with zero Metal dispatch/fallback.

Evidence JSON:

```text
/private/tmp/colmapkit-final-b4-correct.94oTYj/evidence.json
/private/tmp/colmapkit-merged-b4.dbifP0/evidence.json
/private/tmp/colmapkit-merged-w1.FSqtVW/evidence.json
```

## Apple artifact

- XCFramework: `/Users/brw/Developer/ai-projects/colmap/dist/colmapkit-upstream-integration-local/ColmapKit.xcframework`
- Framework bytes: 136,192,000
- Zip: `/Users/brw/Developer/ai-projects/colmap/dist/colmapkit-upstream-integration-local/ColmapKit.xcframework.zip`
- Zip bytes: 42,834,033
- SHA-256 and SwiftPM checksum: `929841cf2ce8a87b4b03c44781e1a50427ab1b5dc291ce5cc9a9abed43e6193e`
- Artifact source identity: `fd0d8637a822672337e0ea51bd376e5ca80746a7`, clean source patch SHA-256 `e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855`
- Slices: macOS arm64 minimum 15.0, iOS device arm64 minimum 18.0, iOS Simulator arm64 minimum 18.0
- vcpkg source: `/private/tmp/colmap-vcpkg-upstream-20260818` at `a0b1c8d3a477c1cb4813d8e127a56961707ca42b`

The macOS framework and bundled `libomp.dylib` both declare macOS 15.0. The framework loads only platform frameworks/system libraries plus `@loader_path/Frameworks/libomp.dylib`; that runtime itself loads only `/usr/lib/libSystem.B.dylib`. Audits found no Homebrew, `/usr/local`, macOS-26 deployment requirement, or host-path leakage. All frameworks and the final XCFramework pass signature verification. Swift import/link probes pass for every slice, and the archive checksum matches the SwiftPM checksum.

Canonical build command:

```bash
env \
  COLMAPKIT_APPLE_BUILD_ROOT=/Users/brw/Developer/ai-projects/colmap/build-colmapkit-apple-sceneprep \
  COLMAPKIT_APPLE_DIST_ROOT=/Users/brw/Developer/ai-projects/colmap/dist/colmapkit-upstream-integration-local \
  COLMAPKIT_VCPKG_ROOT=/private/tmp/colmap-vcpkg-upstream-20260818 \
  COLMAPKIT_CMAKE_TOOLCHAIN_FILE=/private/tmp/colmap-vcpkg-upstream-20260818/scripts/buildsystems/vcpkg.cmake \
  COLMAPKIT_CMAKE_MAKE_PROGRAM=/opt/anaconda3/bin/ninja \
  COLMAPKIT_SOURCE_REVISION=fd0d8637a822672337e0ea51bd376e5ca80746a7 \
  COLMAPKIT_BUILD_LIBOMP=OFF \
  MACOS_DEPLOYMENT_TARGET=15.0 \
  IOS_DEPLOYMENT_TARGET=18.0 \
  bash scripts/build_colmapkit_apple_xcframework.sh
```

## Simulator runtime proof

The corrected harness passed on iPhone 16 Pro Simulator `78E7D416-0021-432B-9ADC-46516DB89050`. It exercised legacy reconstruction, invalid input, async cancellation, point filtering, cropping, conversion, tracked-pose V2, tracked cancellation, and RGB prior.

Key results: 8/8 legacy registered images, 606 legacy sparse points, 2,449 observations, 0.355610 mean reprojection error, cancellation status 4 in 0.0211 seconds, 645 tracked-pose sparse points with no fallback, SH degree 0, 3,825 prior Gaussians, 5.93x densification, valid PLY, and unchanged pose manifest.

Result: `dist/colmapkit-upstream-integration-local/runtime-simulator/result.json`

Command:

```bash
env \
  COLMAPKIT_XCFRAMEWORK_PATH=/Users/brw/Developer/ai-projects/colmap/dist/colmapkit-upstream-integration-local/ColmapKit.xcframework \
  COLMAPKIT_RUNTIME_BUILD_ROOT=/Users/brw/Developer/ai-projects/colmap/build-colmapkit-upstream-integration-runtime \
  COLMAPKIT_RUNTIME_RESULT_ROOT=/Users/brw/Developer/ai-projects/colmap/dist/colmapkit-upstream-integration-local/runtime-simulator \
  IOS_DEPLOYMENT_TARGET=18.0 \
  bash scripts/run_colmapkit_ios_runtime_harness.sh
```

## Speedy Splats local-candidate handoff

Speedy Splats should consume this artifact only through its explicit local candidate seam. The published v0.2.1 artifact (`518f912c564f48b88d53144d15890b722fca9749c8d1db7e80999b7af061fe4d`) and the four-worker Standard COLMAP product policy remain the control. No package pin or product-default change is authorized by this handoff.

The consumer should expect the unchanged released symbols, 136/1,088 legacy struct prefixes, unchanged callback/input-lifetime contract, and default-off Metal paths. The only known logical output shift is the explicitly attributed geometric-verification delta above. The artifact has all three compatible slices and may be copied or locally referenced at the path/checksum in this document.

Remaining gates are physical M1 iPad runtime/performance/thermal proof, product-level quality acceptance, consumer pin approval, publication, tag/release, and any CI validation. None is implied by the local package or Simulator result.
