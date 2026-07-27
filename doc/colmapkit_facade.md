# ColmapKit Facade Notes

Date: 2026-07-27

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

The sparse reconstruction operation is:

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

### Sparse-model post-processing

The synchronous sparse-model post-processing surface is:

```c
ColmapKitStatus ColmapKitRunPointFiltering(
    const ColmapKitPointFilteringConfig* config,
    ColmapKitPointFilteringResult* result);

ColmapKitStatus ColmapKitRunModelCropping(
    const ColmapKitModelCroppingConfig* config,
    ColmapKitModelCroppingResult* result);

ColmapKitStatus ColmapKitRunModelConversion(
    const ColmapKitModelConversionConfig* config,
    ColmapKitModelConversionResult* result);
```

The entry points are short, blocking operations. They do not create job handles,
threads, or cancellation state.

`ColmapKitRunPointFiltering` matches `colmap point_filtering` by applying the
same operations in the same order:

1. `ObservationManager::FilterAllPoints3D(max_reproj_error, min_tri_angle)`.
2. `ObservationManager::FilterPoints3DWithShortTracks(min_track_len)`.

`ColmapKitRunModelCropping` matches the six-coordinate
`colmap model_cropper --boundary x1,y1,z1,x2,y2,z2` path. The C ABI deliberately
uses typed `min_x`, `min_y`, `min_z`, `max_x`, `max_y`, and `max_z` fields
instead of re-parsing the comma-separated boundary string currently assembled
by Splats. It writes the cropped binary model plus the CLI-compatible
`bbox_aligned.txt` and `bbox_oriented.txt` files. The CLI's percentile boundary
form and GPS-transform form are not exposed.

`ColmapKitRunModelConversion` matches `colmap model_converter` for `TXT` output.
It also supports `BIN` output because the reverse conversion is the same
read/write path. Other converter formats remain outside the narrow facade.

Every new config and result has `struct_size` as its first field. Results include
the status, input/output point counts, input/output registered-image counts,
operation-specific counters, and a fixed-capacity
`message[COLMAPKIT_MESSAGE_CAPACITY]`. Paths remain UTF-8 C strings. The facade
validates required paths, complete sparse-model inputs, filter values, crop
bounds, and conversion type before entering the COLMAP operation.

## Intentional Non-Surface

The facade does not expose:

- raw COLMAP C++ objects
- `OptionManager` or CLI argument parsing
- Qt, OpenGL, SiftGPU, GUI, MVS, pycolmap, or ONNX surfaces
- direct database mutation APIs
- bundle adjustment tuning beyond a small set of sparse defaults
- GPS or percentile crop boundaries
- NVM, Bundler, VRML, PLY, R3D, or CAM conversion
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

### Post-processing parity

Validated fixture parity:

```bash
cmake --build build-mise-metal-tests \
  --target colmap_colmapkit_model_postprocessing_test
ctest --test-dir build-mise-metal-tests \
  -R '^colmapkit/model_postprocessing_test$' \
  --output-on-failure
```

Result on 2026-07-27:

```text
1/1 Test #136: colmapkit/model_postprocessing_test ... Passed
100% tests passed, 0 tests failed out of 1
```

The test executable contains five fixture tests. It builds a deterministic
binary sparse model with one rig, four registered frames, twelve points, one
short track, and one deliberately high-error observation. It invokes the real
CLI command implementations and the C ABI on the same input, then verifies:

- `point_filtering`: identical retained point identifiers, coordinates, and
  track lengths.
- `model_cropper`: identical retained points and byte-identical
  `bbox_aligned.txt` / `bbox_oriented.txt`.
- `model_converter --output_type TXT`: byte-identical `rigs`, `cameras`,
  `frames`, `images`, and `points3D` text files.
- The free reverse `TXT` to `BIN` path: byte-identical binary model files.
- Invalid paths, inverted bounds, and unsupported conversion types return a
  status plus a human-readable message.

### Framework compile and symbol evidence

The macOS framework target compiles the new implementation:

```bash
cmake --build build-codex-metal --target ColmapKit -j2
```

The built macOS slice exports:

```text
_ColmapKitRunPointFiltering
_ColmapKitRunModelCropping
_ColmapKitRunModelConversion
```

The current strict iOS probe was also rerun with build mode enabled:

```bash
COLMAPKIT_IOS_BUILD=ON \
COLMAPKIT_IOS_FAIL_ON_BLOCKER=ON \
bash scripts/probe_colmapkit_ios.sh
```

Both `ios-arm64` and `ios-simulator-arm64` currently stop at configure because
the packaging lane's iOS-compatible Boost/dependency prefix is absent. The new
source therefore has not yet been compiled into or symbol-audited in an iOS
framework slice in this checkout. Packaging must restore its iOS dependency
toolchain, rerun both slice builds, and use `nm -gU` on each resulting framework
binary to verify the three symbols above before assembling a replacement
XCFramework.

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
