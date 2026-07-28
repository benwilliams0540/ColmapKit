# ColmapKit Packaging Evidence

Date: 2026-07-27

This matrix tracks the current Apple packaging state for COLMAP's narrow
`ColmapKit` framework surface. The combined macOS arm64, iPhoneOS arm64, and
iPhoneSimulator arm64 XCFramework now contains the sparse-model filtering,
cropping, and conversion ABI required by Splats Scene Prep.

## Current Artifacts

Combined Apple package artifact and audits:

```text
dist/colmapkit-apple-sceneprep/ColmapKit.xcframework
dist/colmapkit-apple-sceneprep/ColmapKit.xcframework.zip
dist/colmapkit-apple-sceneprep/artifact-summary.txt
dist/colmapkit-apple-sceneprep/audits/
dist/colmapkit-apple-sceneprep/runtime-simulator/
```

The iOS artifact, probe logs, audits, and smoke program are generated outputs
and are intentionally ignored by Git. Reproduce them with:

```bash
COLMAPKIT_VCPKG_ROOT=/private/tmp/colmap-vcpkg-sceneprep-20260727 \
X_VCPKG_REGISTRIES_CACHE="$HOME/.cache/vcpkg/registries" \
VCPKG_DEFAULT_BINARY_CACHE="$HOME/.cache/vcpkg/archives" \
bash scripts/build_colmapkit_apple_xcframework.sh
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
| iOS device framework | Proven at build/link/sign/install time | The packaged binary is Mach-O arm64 with `platform IOS`, minOS 18.0, SDK 26.5, public header, and module map. A Swift client referencing all four app-facing operations compiled, linked, developer-signed, and installed on an M1 iPad. Launch was denied because the iPad was locked, and the temporary app was removed. | Rerun the same harness with the device unlocked. |
| iOS simulator framework | Proven at runtime | The packaged binary is Mach-O arm64 with `platform IOSSIMULATOR`, minOS 18.0, SDK 26.5, public header, and module map. The deterministic eight-image runtime harness passed reconstruction, filtering, cropping, TXT conversion, readback, input immutability, invalid-input, and cancellation checks. | Keep the runtime harness in every Scene Prep release audit. |
| XCFramework metadata | Proven | `xcodebuild -create-xcframework` exits zero and `Info.plist` records exactly `macos-arm64`, `ios-arm64`, and `ios-arm64-simulator`, with the correct Simulator variant. | Preserve this slice matrix in published replacements. |
| iOS runtime dependency closure | Proven at Mach-O audit level | Both slice binaries link only allowed Apple frameworks/libraries and `@rpath/ColmapKit`; the audit rejects IOKit, host Homebrew/Conda paths, build/vcpkg paths, and macOS-style `.framework/Versions/` load commands. | Repeat the audit for every release artifact. |
| Swift module import and link | Proven for all packaged platforms | Generated Swift clients import `ColmapKit`, reference sparse reconstruction plus filtering, cropping, and conversion, and compile/link for macOS arm64, generic iPhoneOS arm64, and iPhoneSimulator arm64. | Repeat for every published archive. |
| Exported facade symbols | Proven at binary audit level | All three binaries export identical ColmapKit symbols, including `ColmapKitRunSparseReconstruction`, `ColmapKitRunPointFiltering`, `ColmapKitRunModelCropping`, and `ColmapKitRunModelConversion`. | Reject packaging when any required symbol is missing. |
| Public header and module map | Proven | Headers and module maps are byte-identical across all slices; each header declares the four app-facing entry points and each module map exposes the `colmapkit.h` umbrella header. | Keep the byte comparison and declaration checks in the packager. |
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

- execution on a signed physical iPhone or iPad
- representative sparse reconstruction quality and memory behavior on iOS
- background/relaunch lifecycle behavior in a consumer app
- no-fallback Metal matching or SiftMetal extraction on iOS
- App Store archive, signing, submission, and review acceptance
