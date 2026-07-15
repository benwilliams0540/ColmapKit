# ColmapKit Facade Notes

Date: 2026-06-30

`ColmapKit` is the first native-library seam for embedding COLMAP sparse
reconstruction in Apple apps such as Splats. It is intentionally a narrow C ABI
instead of a Swift-facing C++ API.

For this evidence slice, the authoritative Metal/runtime status is in
[colmapkit_metal_runtime.md](colmapkit_metal_runtime.md), and the current default
recommendation is:

- Proven: CPU ColmapKit sparse reconstruction parity (synthetic fixture metrics match CLI).
- Not yet proven: non-fallback Metal matcher execution in this packaging/runtime slice.
- Pending: SiftMetal extraction remains experimental and is not the Splats default embedded path.
- Planned default embedded path: CPU SIFT + CPU matching, then evaluate Metal matching once non-fallback execution is proven.

## Implemented Surface

Header:

```text
src/colmap/colmapkit/colmapkit.h
```

Library target:

```text
colmap_colmapkit
```

Link-validation sample:

```text
colmapkit_sparse_reconstruct
```

The first operation is:

```c
ColmapKitStatus ColmapKitRunSparseReconstruction(
    const ColmapKitSparseReconstructionConfig* config,
    ColmapKitSparseReconstructionResult* result);
```

It runs:

1. SIFT feature extraction.
2. Sequential, exhaustive, or spatial matching.
3. Incremental mapping through `RunIncrementalMapperImpl`.
4. Optional sparse text export alongside COLMAP's sparse binary output.

The result reports model count, largest model index, registered image count,
sparse point count, observation count, mean reprojection error, and a fixed-size
status message.

Progress is callback-based and intentionally coarse for the first pass:

- preparing
- feature extraction
- matching
- mapping
- sparse text export
- finished
- failed

The callback message pointers are valid only during the callback invocation.
Callers that need to keep messages must copy them.

## Intentional Non-Surface

The facade does not expose:

- raw COLMAP C++ objects
- `OptionManager` or CLI argument parsing
- Qt, OpenGL, SiftGPU, GUI, MVS, pycolmap, or ONNX surfaces
- direct database mutation APIs
- bundle adjustment tuning beyond a small set of sparse defaults
- ownership of process-global logging policy beyond optional
  `ColmapKitInitialize`

This keeps the Swift boundary small enough for an XCFramework and leaves room to
change internal COLMAP calls without breaking Splats.

## Current Build Evidence

Validated target build:

```bash
cmake --build build-codex-metal --target colmapkit_sparse_reconstruct
```

Validated sample help path:

```bash
build-codex-metal/src/colmap/colmapkit/colmapkit_sparse_reconstruct --help
```

Validated error path:

```bash
build-codex-metal/src/colmap/colmapkit/colmapkit_sparse_reconstruct \
  --database_path /private/tmp/colmapkit-smoke/database.db \
  --image_path /path/that/does/not/exist \
  --output_path /private/tmp/colmapkit-smoke/sparse
```

Result:

```text
image_path must be an existing directory.
```

Validated CLI-vs-ColmapKit fixture comparison:

```bash
python3 scripts/python/colmapkit_compare.py \
  --colmap-bin build-codex-metal/src/colmap/exe/colmap \
  --colmapkit-bin \
    build-codex-metal/src/colmap/colmapkit/colmapkit_sparse_reconstruct \
  --run-dir /private/tmp/colmapkit-compare-run \
  --generate-synthetic-fixture \
  --force
```

Result:

```text
Comparison passed.
```

The generated 8-image synthetic fixture produced exact CPU parity for the compared
database and sparse model metrics:

- images: 8
- keypoints/descriptors: 54130
- verified pairs: 17
- verified inliers: 3615
- sparse models: 1
- registered images: 8
- sparse points: 612
- observations: 2453
- mean reprojection error: 0.3380671744135806

Validated macOS framework package:

```bash
COLMAPKIT_CMAKE_TOOLCHAIN_FILE=/private/tmp/colmap-vcpkg/scripts/buildsystems/vcpkg.cmake \
COLMAPKIT_VCPKG_TARGET_TRIPLET=arm64-osx-release-macos15 \
COLMAPKIT_VCPKG_INSTALLED_DIR=/private/tmp/colmap-vcpkg-installed-macos15 \
COLMAPKIT_CMAKE_MAKE_PROGRAM=/private/tmp/colmap-vcpkg/downloads/tools/ninja-1.13.2-osx/ninja \
COLMAPKIT_IGNORE_PREFIXES='/opt/homebrew;/usr/local' \
LIBOMP_ROOT="$PWD/dist/libomp-macos15.0" \
scripts/build_colmapkit_xcframework.sh
```

This produces:

```text
dist/colmapkit/ColmapKit.xcframework
dist/colmapkit/ColmapKit-otool-L.txt
dist/colmapkit/ColmapKit-codesign.txt
dist/colmapkit/ColmapKit-deployment-targets.txt
dist/colmapkit/ColmapKit-deployment-mismatches.txt
```

Validated Swift import and call:

```bash
xcrun swift -module-cache-path .build/swift-module-cache \
  -F dist/colmapkit/ColmapKit.xcframework/macos-arm64 \
  -framework ColmapKit \
  -e 'import ColmapKit; print(String(cString: ColmapKitVersion()))'
```

Result:

```text
COLMAP 4.2.0.dev0 (Commit 2918211e on 2026-07-01 without CUDA)
```

## Packaging Evidence and Remaining Blockers

The current blocker matrix is maintained in:

```text
doc/colmapkit_packaging_blockers.md
```

The macOS arm64 package is self-contained for the current Splats target policy.
The dependency closure is built with the checked-in vcpkg triplet
`cmake/vcpkg-triplets/arm64-osx-release-macos15.cmake`, and OpenMP is supplied by
`scripts/build_libomp_macos.sh` instead of the Homebrew `libomp` bottle.

Current `otool -L` evidence shows only system frameworks/libraries plus the
vendored runtime:

```text
@loader_path/Frameworks/libomp.dylib
```

Current deployment evidence:

```text
ColmapKit minOS: 15.0
Vendored libomp.dylib minOS: 15.0
Deployment mismatches: 0
```

Current signing verification:

```text
dist/colmapkit/ColmapKit.xcframework: valid on disk
dist/colmapkit/ColmapKit.xcframework: satisfies its Designated Requirement
```

## iOS Probe Evidence

Validated build and package command:

```bash
COLMAPKIT_IOS_BUILD=ON \
COLMAPKIT_IOS_USE_VCPKG=ON \
COLMAPKIT_IOS_FAIL_ON_BLOCKER=ON \
COLMAPKIT_VCPKG_ROOT=/private/tmp/colmap-vcpkg \
COLMAPKIT_VCPKG_INSTALLED_DIR=/private/tmp/colmap-vcpkg-installed-ios-framework \
bash scripts/probe_colmapkit_ios.sh
```

This writes:

```text
dist/colmapkit-ios-probe/summary.md
dist/colmapkit-ios-probe/ios-arm64-configure.log
dist/colmapkit-ios-probe/ios-arm64-build.log
dist/colmapkit-ios-probe/ios-simulator-arm64-configure.log
dist/colmapkit-ios-probe/ios-simulator-arm64-build.log
dist/colmapkit-ios/ColmapKit.xcframework
dist/colmapkit-ios/framework-audit.txt
dist/colmapkit-ios/xcframework-info.txt
dist/colmapkit-ios/smoke/colmapkit-smoke
```

The probe intentionally defaults to strict dependency mode, which ignores local
macOS package prefixes such as `/opt/homebrew`, `/usr/local`, and
`/opt/anaconda3`. This prevents a false-positive iOS configure that accidentally
links host macOS libraries.

Current result:

- arm64 iPhoneOS slice: configure, compile, and link pass.
- arm64 iPhoneSimulator slice: configure, compile, and link pass.
- the XCFramework contains exactly those two slices with minOS 18.0.
- both Mach-O dependency audits reject host/macOS-only paths and frameworks.
- a generated arm64 Simulator Swift program imports the module, references
  `ColmapKitVersion`, and links successfully against the XCFramework.

This proves framework build form, module import, and link closure. Simulator
execution, physical-device execution, representative reconstruction, and
no-fallback Metal execution remain separate runtime gates.
