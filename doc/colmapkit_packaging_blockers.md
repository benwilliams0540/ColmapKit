# ColmapKit Packaging Blockers

Date: 2026-07-01

This matrix tracks blockers found while shaping COLMAP into an Apple-facing
`ColmapKit.xcframework` for Splats.

## Current Artifacts

macOS proof package:

```text
dist/colmapkit/ColmapKit.xcframework
dist/colmapkit/ColmapKit-otool-L.txt
dist/colmapkit/ColmapKit-codesign.txt
dist/colmapkit/ColmapKit-deployment-targets.txt
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
| macOS framework shape | Proven locally | `bash scripts/build_colmapkit_xcframework.sh` creates a macOS arm64 `ColmapKit.xcframework`. | Keep hardening the package script; Splats integration remains gated on the deployment-target blocker below. |
| Swift import | Proven locally | Swift can import `ColmapKit` and call `ColmapKitVersion` with `-F dist/colmapkit/ColmapKit.xcframework/macos-arm64`. | Add a small Splats-side wrapper target when editing Splats. |
| Runtime dependency closure | Path closure proven; shipping still blocked | `dist/colmapkit/ColmapKit-otool-L.txt` rewrites direct non-system links to `@loader_path/Frameworks/...`; a recursive `otool -L` scan over `ColmapKit` and the 71 vendored dylibs shows no `/opt/homebrew` or `/usr/local` paths. | Do not vendor into Splats yet; resolve the deployment-target mismatch first or replace this wide dylib bundle with a slimmer/static dependency set. |
| Deployment target | Blocked | `dist/colmapkit/ColmapKit-deployment-targets.txt` shows `ColmapKit` at minOS 13.0, but 63 vendored dylibs at minOS 26.0. The linker also warns about macOS 26.0 Homebrew dylibs while targeting macOS 13.0. | Rebuild dependencies for the target deployment version, slim to a smaller reconstruction dependency closure, or intentionally raise Splats' ColmapKit support floor before vendoring. |
| Signing | Proven locally; not sufficient for shipping | `dist/colmapkit/ColmapKit-codesign.txt` records `codesign --verify --deep --strict --verbose=2 dist/colmapkit/ColmapKit.xcframework` as valid and satisfying its designated requirement. | Keep signing in the package script, but do not treat a signed artifact with minOS 26.0 vendored dylibs as shippable for the current macOS target. |
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
bundled, signed, versioned with the app, and built for the same deployment
target policy as Splats. The current bundled-dylib package is self-contained
and signed, but remains blocked because most vendored dylibs require macOS
26.0.
