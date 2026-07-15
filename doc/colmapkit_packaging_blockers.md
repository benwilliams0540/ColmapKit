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
COLMAPKIT_IOS_BUILD=OFF COLMAPKIT_IOS_USE_VCPKG=ON COLMAPKIT_IOS_FAIL_ON_BLOCKER=ON COLMAPKIT_VCPKG_ROOT=/private/tmp/colmap-vcpkg COLMAPKIT_VCPKG_INSTALLED_DIR=/private/tmp/colmap-vcpkg-installed-ios-gklib bash scripts/probe_colmapkit_ios.sh
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
| iOS vcpkg dependency route | Partial; GKlib and Metis proven | The repository-owned `cmake/vcpkg-ports/gklib` overlay preserves the pinned GKlib 2023 port and replaces only the `TARGET_OS_IPHONE` implementations of `gk_mkpath` and `gk_rmpath` with direct filesystem calls. The configure-only manifest probe builds and installs GKlib 2023-03-27 and Metis 2022-07-27 for both iOS triplets. | Keep the overlay enabled for both triplets while later dependency blockers are investigated. |
| iOS device dependencies | Blocked after GKlib/Metis | `dist/colmapkit-ios-probe/ios-arm64-configure.log` records successful `gklib:arm64-ios-release` and `metis:arm64-ios-release` builds, then fails in OpenColorIO 2.5.2 because `SystemMonitor_macos.cpp` includes `IOKit/graphics/IOGraphicsLib.h`, which is absent from the iPhoneOS 26.5 SDK. | Treat OpenColorIO's macOS-only system-monitor path as the next dependency-portability slice. |
| iOS simulator dependencies | Blocked after GKlib/Metis | `dist/colmapkit-ios-probe/ios-simulator-arm64-configure.log` records successful `gklib:arm64-ios-simulator-release` and `metis:arm64-ios-simulator-release` builds, then hits the same missing IOKit graphics header under the iPhoneSimulator 26.5 SDK. | Carry any future OpenColorIO fix through both triplets. |
| COLMAP iOS configure | Not reached | vcpkg manifest installation stops at OpenColorIO before COLMAP's own dependency discovery and generation complete. | Do not run or claim a ColmapKit framework build until both configurations finish. |
| ColmapKit iOS framework build | Not attempted | The validated probe used `COLMAPKIT_IOS_BUILD=OFF`, and both configurations failed before generating build files. | After the OpenColorIO dependency route clears, rerun configure-only first and then opt into `COLMAPKIT_IOS_BUILD=ON`. |
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

For macOS, Homebrew bottles are acceptable only as proof-of-concept build input.
The shippable package path uses static vcpkg dependencies built with
`cmake/vcpkg-triplets/arm64-osx-release-macos15.cmake` and a source-built
`libomp.dylib` from `scripts/build_libomp_macos.sh`. The package script enforces
the deployment policy by failing by default when any vendored dylib requires a
newer macOS version than `MACOS_DEPLOYMENT_TARGET`.
