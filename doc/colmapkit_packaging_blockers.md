# ColmapKit Packaging Blockers

Date: 2026-07-15

This matrix tracks blockers found while shaping COLMAP into an Apple-facing
`ColmapKit.xcframework` for Splats.

## Current Artifacts

macOS package artifact:

```text
dist/colmapkit/ColmapKit.xcframework
```

Generated package audits:

```text
dist/colmapkit/ColmapKit-otool-L.txt
dist/colmapkit/ColmapKit-codesign.txt
dist/colmapkit/ColmapKit-deployment-targets.txt
dist/colmapkit/ColmapKit-deployment-mismatches.txt
```

iOS probe summary:

```text
dist/colmapkit-ios-probe/summary.md
```

Latest vcpkg-backed probe command:

```text
COLMAPKIT_IOS_BUILD=OFF COLMAPKIT_IOS_USE_VCPKG=ON COLMAPKIT_IOS_FAIL_ON_BLOCKER=ON COLMAPKIT_VCPKG_ROOT=/private/tmp/colmap-vcpkg COLMAPKIT_VCPKG_INSTALLED_DIR=/private/tmp/colmap-vcpkg-installed-ios-ocio bash scripts/probe_colmapkit_ios.sh
```

The intended macOS `ColmapKit.xcframework` is staged through Git LFS when it is
ready for Splats vendoring. The audit files and iOS probe outputs remain
generated artifacts and are intentionally ignored by Git.

For the runtime split and fallback policy, see [ColmapKit Metal Runtime Decision Memo](colmapkit_metal_runtime.md).

## Blocker Matrix

| Category | Status | Evidence | Next Action |
| --- | --- | --- | --- |
| macOS framework shape | Proven | The package script creates a macOS arm64 `ColmapKit.xcframework`, signs it, writes audits, and exits zero with the vcpkg macOS 15 triplet plus target-compatible libomp. | Vendor the artifact into Splats and keep the external CLI fallback available. |
| Swift import | Proven | Swift can import `ColmapKit` and call `ColmapKitVersion` with `-F dist/colmapkit/ColmapKit.xcframework/macos-arm64`; latest result: `COLMAP 4.2.0.dev0 (Commit 2918211e on 2026-07-01 without CUDA)`. | Add the Splats-side wrapper target and adapter. |
| Runtime dependency closure | Proven for macOS arm64 | `dist/colmapkit/ColmapKit-otool-L.txt` shows only system frameworks/libraries plus `@loader_path/Frameworks/libomp.dylib`; the static vcpkg dependency closure removes the previous 71-dylib Homebrew bundle. | Preserve the vcpkg/static dependency path for release packages. |
| Deployment target | Proven for macOS 15 | `dist/colmapkit/ColmapKit-deployment-targets.txt` records `ColmapKit` at minOS 15.0 and bundled `libomp.dylib` at minOS 15.0; `ColmapKit-deployment-mismatches.txt` has 0 lines. | Keep the hard mismatch gate enabled by default. |
| Signing | Proven | `dist/colmapkit/ColmapKit-codesign.txt` records `codesign --verify --deep --strict --verbose=2 dist/colmapkit/ColmapKit.xcframework` as valid and satisfying its designated requirement. | Re-sign after any post-package changes. |
| OpenMP runtime | Proven for macOS 15 | `scripts/build_libomp_macos.sh` builds LLVM OpenMP from source with `MACOS_DEPLOYMENT_TARGET=15.0`; the package script vendors and rewrites `@rpath/libomp.dylib` to `@loader_path/Frameworks/libomp.dylib`. | Use the source-built runtime, not Homebrew's macOS 26 bottle, for Splats packages. |
| iOS vcpkg dependency route | Manifest closure proven for device and simulator | The repository overlays preserve the pinned GKlib 2023 and OpenColorIO 2.5.2 registry ports while applying iOS-only portability patches. The fresh configure-only probe reports `All requested installations completed successfully` for both iOS triplets, including GKlib 2023-03-27, Metis 2022-07-27, OpenColorIO 2.5.2, and OpenImageIO 3.1.14.0. | Keep both overlays enabled while COLMAP's post-install configuration blockers are investigated. |
| OpenColorIO system monitors | Proven for both iOS triplets; macOS behavior preserved | `cmake/vcpkg-ports/opencolorio/ios-system-monitor.diff` routes `TARGET_OS_IPHONE` through OpenColorIO's empty monitor implementation. Direct installs succeed for `arm64-ios-release`, `arm64-ios-simulator-release`, and `arm64-osx-release-macos15`; the iOS archives have no desktop monitor symbols, while the macOS archive still references Core Graphics, ColorSync, and IOKit monitor APIs. | Preserve the explicit platform split when updating the pinned registry port. |
| iOS device dependencies | Proven through manifest installation | `dist/colmapkit-ios-probe/ios-arm64-configure.log` records successful OpenColorIO 2.5.2 and OpenImageIO 3.1.14.0 builds and completes all requested vcpkg installations before COLMAP configuration fails at OpenGL discovery. | Treat COLMAP's unconditional OpenGL/GLEW discovery as the next device configuration slice. |
| iOS simulator dependencies | Proven through manifest installation | `dist/colmapkit-ios-probe/ios-simulator-arm64-configure.log` records the same successful OpenColorIO and OpenImageIO builds for `arm64-ios-simulator-release`, then reaches the same OpenGL discovery failure. | Carry the next configuration change through both triplets. |
| COLMAP iOS configure | Reached; blocked at OpenGL discovery | Although the probe passes `OPENGL_ENABLED=OFF` and `GUI_ENABLED=OFF`, `cmake/FindDependencies.cmake` unconditionally calls `find_package(OpenGL)` and fails with missing `OPENGL_gl_LIBRARY` and `OPENGL_INCLUDE_DIR` for both iOS SDKs. Generation does not complete. | Decide how dependency discovery should honor the disabled OpenGL path; do not start a framework build before both configurations generate successfully. |
| ColmapKit iOS framework build | Not attempted | The validated probe used `COLMAPKIT_IOS_BUILD=OFF`, and both configurations stopped before generating build files. | After both configure-only slices complete, opt into `COLMAPKIT_IOS_BUILD=ON`. |
| Metal matching in package | Not runtime-proven | Strict comparison requests Metal but still logs `Requested Metal SIFT descriptor matching, but the Metal backend is unavailable or failed at runtime; falling back to deterministic CPU matching.` (`--force` mode fails by design). | Keep `use_metal_matching=0` for Splats embedded default until end-to-end non-fallback execution is demonstrated. |
| SiftMetal extraction resources | Unknown | `SIFT_METAL_ENABLED=OFF` in the package script. | If SiftMetal ships, bundle `sift.metallib` inside `ColmapKit.framework` and load it from that bundle. |

## Dependency Policy

Do not treat host Homebrew or Conda libraries as iOS evidence. The iOS probe
defaults to strict dependency mode and ignores `/opt/homebrew`, `/usr/local`,
and `/opt/anaconda3` so that any successful iOS configure must come from an
iOS-compatible dependency prefix or toolchain.

Use `COLMAPKIT_IOS_USE_VCPKG=ON` with `COLMAPKIT_VCPKG_ROOT` pointing at a
bootstrapped vcpkg checkout to exercise the current iOS route. The probe passes
`VCPKG_MANIFEST_NO_DEFAULT_FEATURES=ON` so GUI/download dependencies do not
inflate the iOS proof, and it uses the local `cmake/vcpkg-triplets` overlays to
pin the iOS 18 deployment target.

The probe also defaults `VCPKG_OVERLAY_PORTS` to `cmake/vcpkg-ports`. Its GKlib
overlay retains the registry port's pinned source and existing patches. On iOS,
`gk_mkpath` uses `mkdir(2)` recursively and `gk_rmpath` uses
`opendir(3)`, `readdir(3)`, `unlink(2)`, and `rmdir(2)` instead of spawning
shell commands through unavailable `system(3)`. The original desktop
implementations remain under the non-iOS preprocessor branch; the overlay also
builds with the repository's macOS 15 vcpkg triplet.

The OpenColorIO overlay mirrors the vcpkg registry's 2.5.2 port and existing
patches, then adds only `ios-system-monitor.diff`. The patch includes
`TargetConditionals.h` and treats `TARGET_OS_IPHONE` as having no enumerable
system monitors, avoiding the macOS-only `IOGraphicsLib.h` path without
disabling OpenColorIO's color-processing library. The non-iOS Apple branch is
unchanged. Direct overlay installs were validated for `arm64-ios-release`,
`arm64-ios-simulator-release`, and `arm64-osx-release-macos15` before the full
manifest probe was rerun.

For macOS, Homebrew bottles are acceptable only as proof-of-concept build input.
The shippable package path uses static vcpkg dependencies built with
`cmake/vcpkg-triplets/arm64-osx-release-macos15.cmake` and a source-built
`libomp.dylib` from `scripts/build_libomp_macos.sh`. The package script enforces
the deployment policy by failing by default when any vendored dylib requires a
newer macOS version than `MACOS_DEPLOYMENT_TARGET`.
