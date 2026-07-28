# ColmapKit Packaging Evidence

Date: 2026-07-15

This matrix tracks the current Apple packaging state for COLMAP's narrow
`ColmapKit` framework surface. The first arm64 iPhoneOS and arm64
iPhoneSimulator XCFramework is now available for integration testing; runtime
behavior on an Apple device or in Simulator is not implied by the build proof.

## Current Artifacts

macOS package artifact and audits:

```text
dist/colmapkit/ColmapKit.xcframework
dist/colmapkit/ColmapKit-otool-L.txt
dist/colmapkit/ColmapKit-codesign.txt
dist/colmapkit/ColmapKit-deployment-targets.txt
dist/colmapkit/ColmapKit-deployment-mismatches.txt
```

iOS package artifact and audits:

```text
dist/colmapkit-ios/ColmapKit.xcframework
dist/colmapkit-ios/framework-audit.txt
dist/colmapkit-ios/xcframework-info.txt
dist/colmapkit-ios/smoke/colmapkit-smoke
dist/colmapkit-ios-probe/summary.md
```

The iOS artifact, probe logs, audits, and smoke program are generated outputs
and are intentionally ignored by Git. Reproduce them with:

```bash
COLMAPKIT_IOS_BUILD=ON \
COLMAPKIT_IOS_USE_VCPKG=ON \
COLMAPKIT_IOS_FAIL_ON_BLOCKER=ON \
COLMAPKIT_VCPKG_ROOT=/private/tmp/colmap-vcpkg \
COLMAPKIT_VCPKG_INSTALLED_DIR=/private/tmp/colmap-vcpkg-installed-ios-framework \
bash scripts/probe_colmapkit_ios.sh
```

The validated run used Xcode 26.5 (17F42), iPhoneOS/iPhoneSimulator SDK 26.5,
CMake 4.3.2, vcpkg commit `3e169054dfb52ed75fa3159a81282db4b401ae03`,
and deployment target 18.0. Relevant dependency versions were GKlib 2023-03-27,
Metis 2022-07-27, OpenColorIO 2.5.2, OpenImageIO 3.1.14.0, and FAISS 1.14.1.

For the runtime split and fallback policy, see
[ColmapKit Metal Runtime Decision Memo](colmapkit_metal_runtime.md).

## Evidence Matrix

| Category | Status | Evidence | Next Action |
| --- | --- | --- | --- |
| macOS framework shape | Proven | The existing package path creates and signs a macOS arm64 XCFramework. A fresh arm64 framework regression build after the iOS changes completed with `platform MACOS`, minOS 15.0, normal OpenMP-backed FAISS, and the expected macOS ColorSync/CoreGraphics/IOKit links. | Keep the macOS package validation in place when changing shared CMake or overlays. |
| iOS vcpkg dependency route | Proven for device and simulator builds | Both iOS triplets install the reduced manifest closure from the checked-in overlays, including GKlib, Metis, OpenColorIO, and OpenImageIO, then compile the full ColmapKit target. | Keep the pinned overlays and both triplet builds in the probe. |
| OpenGL discovery | Proven disabled for the embedded path | `FindDependencies.cmake` now skips OpenGL/GLEW discovery when GUI, OpenGL, and CUDA paths are disabled, while retaining discovery for GUI/OpenGL and CUDA/SiftGPU builds. Both iOS configurations generate successfully. | Exercise a full GUI desktop build separately when that surface changes. |
| FAISS without iOS OpenMP | Proven at build/link time | The fetched FAISS build has an explicit OpenMP option. iOS disables it and supplies a serial compatibility header for the small OpenMP API surface used by FAISS; both slices compile and link. macOS keeps FAISS OpenMP enabled. | Treat iOS retrieval/indexing as serial until performance evidence justifies a different backend. |
| OpenColorIO system monitors | Proven platform split | iOS compiles the empty system-monitor implementation and omits macOS-only ColorSync/CoreGraphics/IOKit link dependencies. A macOS overlay regression build retains those frameworks. | Preserve both the source and CMake platform guards when updating OpenColorIO. |
| iOS device framework | Proven at build/link time | `ColmapKit.framework/ColmapKit` is a Mach-O arm64 binary with `platform IOS`, minOS 18.0, SDK 26.5, public header, and module map. | Load and call the API from a signed physical-device test app. |
| iOS simulator framework | Proven at build/link time | `ColmapKit.framework/ColmapKit` is a Mach-O arm64 binary with `platform IOSSIMULATOR`, minOS 18.0, SDK 26.5, public header, and module map. | Run the integration app in an arm64 Simulator. |
| XCFramework metadata | Proven | `xcodebuild -create-xcframework` exits zero and `xcframework-info.txt` records exactly one `ios-arm64` library and one `ios-arm64-simulator` library with the simulator platform variant. | Integrate the generated XCFramework into the consumer project. |
| iOS runtime dependency closure | Proven at Mach-O audit level | Both slice binaries link only allowed Apple frameworks/libraries and `@rpath/ColmapKit`; the audit rejects IOKit, host Homebrew/Conda paths, build/vcpkg paths, and macOS-style `.framework/Versions/` load commands. | Repeat the audit for every release artifact. |
| Swift module import and link | Proven for arm64 Simulator | The generated Swift smoke source imports `ColmapKit`, references `ColmapKitVersion`, and compiles/links into an arm64 iOS Simulator executable against the packaged XCFramework. | Execute the smoke call inside Simulator; compile/link success is not runtime proof. |
| Exported facade symbols | Proven at binary audit level | Both slices export the public C ABI, including `ColmapKitVersion`, lifecycle functions, and sparse reconstruction entry points. | Add consumer-side lifecycle and cancellation tests. |
| Metal matching in package | Not runtime-proven | Metal support is compiled, but prior strict runtime comparison fell back to deterministic CPU matching. | Keep CPU matching as the embedded default until no-fallback execution is demonstrated. |
| SiftMetal extraction resources | Not packaged | `SIFT_METAL_ENABLED=OFF` remains the package default. | If enabled later, bundle `sift.metallib` inside `ColmapKit.framework` and validate bundle-relative loading. |

## Dependency Policy

Do not treat host Homebrew or Conda libraries as iOS evidence. The iOS probe
uses strict dependency mode, ignores `/opt/homebrew`, `/usr/local`, and
`/opt/anaconda3`, and builds against SDK-targeted vcpkg triplets with
`VCPKG_MANIFEST_NO_DEFAULT_FEATURES=ON`.

The GKlib overlay retains the registry port's pinned source and desktop path.
On iOS, filesystem operations use `mkdir(2)`, `opendir(3)`, `readdir(3)`,
`unlink(2)`, and `rmdir(2)` rather than unavailable shell commands. The
OpenColorIO overlay uses an empty monitor implementation on iOS and suppresses
only the iOS link to desktop monitor frameworks; the macOS source and link path
remain active.

FAISS defaults to its upstream OpenMP behavior. When `OPENMP_ENABLED=OFF`, as
on iOS, the COLMAP fetch supplies a one-thread OpenMP compatibility header and
disables FAISS OpenMP linking. This is a correctness and portability path, not
a claim of parallel retrieval performance.

For shippable macOS packages, use static vcpkg dependencies and the
target-compatible `libomp.dylib` produced by `scripts/build_libomp_macos.sh`.
The Homebrew libomp bottle is useful only for local compile regression because
its current deployment target is newer than macOS 15.

## Still Unproven

- execution in an arm64 iOS Simulator process
- execution on a signed physical iPhone or iPad
- representative sparse reconstruction quality and memory behavior on iOS
- cancellation and lifecycle behavior in a consumer app
- no-fallback Metal matching or SiftMetal extraction on iOS
- App Store archive, signing, submission, and review acceptance
