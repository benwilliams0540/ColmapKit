# SiftMetal Third-Party Import

This directory contains a narrow Objective-C++/Metal SIFT extractor import based
on the byplay COLMAP Metal fork:

```text
byplay-io COLMAP Metal fork
bf01a458b958fbe31fcb67643c44e873e6ec2dd0
```

The prior-art fork describes the implementation as a port of Luke Van In's
SIFTMetal Swift library, adapted for COLMAP integration. The upstream SIFTMetal
license is MIT; the license text is included in `LICENSE`.

## Contents

- `SiftMetal.h` exposes a small C++ API around grayscale `uint8_t` input,
  SIFT keypoints, and float descriptors.
- `SiftMetal.mm` owns the Metal device, pipelines, textures, and extraction
  flow.
- `include/*.h` contains shared structs used by both Objective-C++ and Metal.
- `shaders/*.metal` and `shaders/Common.hpp` contain the compute kernels.
- `CMakeLists.txt` builds `colmap_sift_metal` and compiles `sift.metallib`.

## Integration Notes

The parent build includes this subtree only when `SIFT_METAL_ENABLED` resolves
true. The runtime loader searches for `sift.metallib` through an environment
override, app bundle resource paths, installed CLI-relative paths, the
build-tree path, and finally Metal's default library.

The implementation is intentionally not wired as a default extractor by this
subtree. COLMAP integration lives in `src/colmap/feature/sift.cc` and remains
behind the explicit `SiftExtraction.use_metal` option.

## Provenance Caveat

SIFTMetal's upstream README says it was based on IPOL SIFT and OpenSIFT. Before
an upstream COLMAP pull request, re-check whether those notices must also be
included because of derivative implementation details, not merely algorithmic
inspiration.
