# ColmapKit Packaging Blockers

Date: 2026-06-30

This matrix tracks blockers found while shaping COLMAP into an Apple-facing
`ColmapKit.xcframework` for Splats.

## Current Artifacts

macOS proof package:

```text
dist/colmapkit/ColmapKit.xcframework
```

iOS probe summary:

```text
dist/colmapkit-ios-probe/summary.md
```

Both paths are generated artifacts and are intentionally ignored by Git.

For the runtime split and fallback policy, see [ColmapKit Metal Runtime Decision Memo](colmapkit_metal_runtime.md).

## Blocker Matrix

| Category | Status | Evidence | Next Action |
| --- | --- | --- | --- |
| macOS framework shape | Proven locally | `bash scripts/build_colmapkit_xcframework.sh` creates a macOS arm64 `ColmapKit.xcframework`. | Keep hardening the package script while Splats integration starts against macOS. |
| Swift import | Proven locally | Swift can import `ColmapKit` and call `ColmapKitVersion` with `-F dist/colmapkit/ColmapKit.xcframework/macos-arm64`. | Add a small Splats-side wrapper target when editing Splats. |
| Runtime dependency closure | Blocked | `dist/colmapkit/ColmapKit-otool-L.txt` shows Homebrew dylibs for Boost, Ceres, OpenImageIO, glog, gflags, Metis, libomp, and SuiteSparse/CHOLMOD. | Choose static/vendored dependency builds or a deliberate bundled-dylib/signing strategy. |
| Deployment target | Blocked | macOS package link warns that Homebrew dylibs were built for macOS 26.0 while the package targets macOS 13.0. | Rebuild dependencies for the target deployment version or raise the package deployment target intentionally. |
| Signing | Not started | `codesign --verify --deep --strict --verbose=2 dist/colmapkit/ColmapKit.xcframework` reports `code object is not signed at all`. | Sign only after dependency closure is deliberate; do not sign a misleading host-dylib package. |
| OpenMP disabled build | Blocked | COLMAP skips direct OpenMP lookup with `OPENMP_ENABLED=OFF`, but Homebrew `CHOLMODConfig.cmake` still calls `find_dependency(OpenMP COMPONENTS C)`. | Use an OpenMP-capable dependency set or build SuiteSparse/CHOLMOD without OpenMP for the package. |
| iOS device slice | Blocked at configure | `bash scripts/probe_colmapkit_ios.sh` fails in strict mode at Boost discovery for `iphoneos`. | Provide an iOS-compatible dependency prefix/toolchain, then rerun the probe. |
| iOS simulator slice | Blocked at configure | `bash scripts/probe_colmapkit_ios.sh` fails in strict mode at Boost discovery for `iphonesimulator`. | Provide an iOS simulator-compatible dependency prefix/toolchain, then rerun the probe. |
| Later iOS dependencies | Unknown | The strict iOS probe stops at Boost before Eigen, OpenImageIO, Metis, glog, SQLite, CHOLMOD/SuiteSparse, Ceres, PoseLib, and FAISS are tested. | After Boost is available, keep rerunning the probe and promote each new failure into this matrix. |
| Metal matching in package | Not runtime-proven | Strict comparison requests Metal but still logs `Requested Metal SIFT descriptor matching, but the Metal backend is unavailable or failed at runtime; falling back to deterministic CPU matching.` (`--force` mode fails by design). | Keep `use_metal_matching=0` for Splats embedded default until end-to-end non-fallback execution is demonstrated. |
| SiftMetal extraction resources | Unknown | `SIFT_METAL_ENABLED=OFF` in the package script. | If SiftMetal ships, bundle `sift.metallib` inside `ColmapKit.framework` and load it from that bundle. |

## Dependency Policy

Do not treat host Homebrew or Conda libraries as iOS evidence. The iOS probe
defaults to strict dependency mode and ignores `/opt/homebrew`, `/usr/local`,
and `/opt/anaconda3` so that any successful iOS configure must come from an
iOS-compatible dependency prefix or toolchain.

For macOS, Homebrew is acceptable as proof-of-concept build input, but it is
not a shippable dependency closure for Splats unless the dylibs are deliberately
bundled, signed, and versioned with the app.
