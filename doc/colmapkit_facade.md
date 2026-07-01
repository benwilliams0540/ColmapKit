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
bash scripts/build_colmapkit_xcframework.sh
```

This produces:

```text
dist/colmapkit/ColmapKit.xcframework
dist/colmapkit/ColmapKit-otool-L.txt
```

Validated Swift import and call:

```bash
swift -module-cache-path /private/tmp/colmapkit-swift-module-cache \
  -F dist/colmapkit/ColmapKit.xcframework/macos-arm64 \
  -framework ColmapKit \
  -e 'import ColmapKit; print(String(cString: ColmapKitVersion()))'
```

Result:

```text
COLMAP 4.2.0.dev0 (Commit 409bbded on 2026-06-30 without CUDA)
```

## Packaging Evidence and Remaining Blockers

The current blocker matrix is maintained in:

```text
doc/colmapkit_packaging_blockers.md
```

A fresh reduced configure with `OPENMP_ENABLED=OFF` now skips COLMAP's direct
OpenMP lookup, but Homebrew's `CHOLMODConfig.cmake` still calls
`find_dependency(OpenMP COMPONENTS C)` because that CHOLMOD package was built
with OpenMP support. This means the packaging agent must either:

- make OpenMP discoverable and include it in the dependency audit, or
- provide/use a SuiteSparse/CHOLMOD build that does not require OpenMP.

This is a real dependency-closure blocker for an Apple XCFramework build and
should be tracked separately from ColmapKit API work.

The package script works around the local OpenMP lookup by making Homebrew
`libomp` discoverable and avoids local Conda package leakage with
`CMAKE_IGNORE_PREFIX_PATH=/opt/anaconda3`.

The generated macOS framework is a local proof artifact, not a shippable
binary. `otool -L` still reports runtime dependencies on Homebrew dylibs:

- Boost
- Ceres Solver
- OpenImageIO
- glog
- gflags
- Metis
- libomp
- SuiteSparse/CHOLMOD

The link also warns that these Homebrew dylibs were built for macOS 26.0 while
the current package script targets macOS 13.0. A production package needs
either static/vendored dependency closure or a deliberate dependency bundling
and signing strategy.

Signing has not been performed. Current verification result:

```text
dist/colmapkit/ColmapKit.xcframework: code object is not signed at all
```

## iOS Probe Evidence

Validated probe command:

```bash
bash scripts/probe_colmapkit_ios.sh
```

This writes:

```text
dist/colmapkit-ios-probe/summary.md
dist/colmapkit-ios-probe/ios-arm64-configure.log
dist/colmapkit-ios-probe/ios-simulator-arm64-configure.log
```

The probe intentionally defaults to strict dependency mode, which ignores local
macOS package prefixes such as `/opt/homebrew`, `/usr/local`, and
`/opt/anaconda3`. This prevents a false-positive iOS configure that accidentally
links host macOS libraries.

Current result:

- iOS device slice: configure fails.
- iOS simulator slice: configure fails.
- First blocker for both slices: no iOS-compatible Boost CMake package is
  available to the probe.

Representative error:

```text
Could not find a package configuration file provided by "Boost"
```

This probe stops at Boost, so later dependencies such as Eigen, OpenImageIO,
Metis, glog, SQLite, CHOLMOD/SuiteSparse, Ceres, and any iOS-specific Metal
resource behavior remain unproven until an iOS dependency prefix/toolchain is
provided.
