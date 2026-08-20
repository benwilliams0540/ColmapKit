# ColmapKit Standard-COLMAP Metal Experiment Handoff

Date: 2026-08-19

## Decision

The strict experiment is executable and evidence-complete, but no Metal route
is suitable for product promotion. Keep four-worker CPU Standard COLMAP as the
Splats control and default.

- Metal SIFT was 12.01% faster in median total wall time, below the 20% target,
  while producing 16.72% fewer sparse points and 18.38% fewer observations.
  Its downstream matching and mapping reductions therefore cannot be claimed as
  same-work acceleration.
- Metal descriptor matching was 21.94% slower in total and 192.70% slower in
  its matching stage.
- The combined route was 4.40% slower in total and produced 15.61% fewer points
  and 16.91% fewer observations.
- Every macOS result labeled Metal proved the requested Metal backend, positive
  dispatch counts, a named Metal device, and zero fallback. All other strict
  failures remain failures rather than Metal results.

No push, PR, CI, tag, release, package publication, Splats edit, or product
default change was made.

## Source and implementation

- Branch: `integrate/upstream-main-2026-08`
- Upstream-integrated starting point: `b8be21ff5b90477aab31ea7618cfad6b80711ed6`
- Packaged-engine commit: `42f24bd78ceb96b045f072d1b8c311556652497c`
- Apple runtime-harness commit: `ac2dd74803fdc3b72289952cf799461a2dae6be7`
- Upstream merge already present: `fd0d8637a` (upstream
  `d2da19444e7c9f73ae4a5926c2d6710d04b91f03`)

The engine change is deliberately narrow:

- compile the SIFT metallib for each Apple platform target;
- package that metallib in each framework slice;
- resolve it relative to the loaded ColmapKit framework before development-tree
  fallbacks;
- preserve opt-in build and runtime flags;
- add a balanced benchmark driver and strict Simulator/device runtime receipts.

The existing sparse-reconstruction symbol is unchanged. The current 168-byte
config and 1,376-byte result remain tail-additive and `struct_size` guarded.
Released callers continue to use the 136-byte config and 1,088-byte result.
Partial tails fail before feature work. Metal remains default-off.

Earlier strict receipts at `/private/tmp/colmapkit-strict-sift-fd0d8637.json`
and `/private/tmp/colmapkit-strict-matching-fd0d8637.json` failed before useful
feature work because the restricted launch context exposed no default Metal
device. Their exact failure reasons were:

- `Strict Metal SIFT is unavailable: Metal default device is unavailable`
- `Strict Metal matching was requested but no usable Metal matcher device or pipeline is available.`

Normal macOS, Simulator, and physical-iPad launches prove that this was an
execution-context boundary, not silent fallback. A relocated macOS framework
also completed eight strict Metal SIFT dispatches with the build-tree metallib
temporarily unavailable, proving framework-relative resource loading.

## Immutable macOS fixture

- Images: 115 JPEGs under
  `/Users/brw/Documents/Splats Projects/Unsorted Scenes.splatsproject/scenes/control-native-1B9985D8/attempts/starter-20260712-190850/selected-frames`
- Image list:
  `/Users/brw/Documents/Splats Projects/Unsorted Scenes.splatsproject/scenes/control-native-1B9985D8/attempts/starter-20260712-190850/manifests/colmap-image-list.txt`
- Live filename/size/content manifest SHA-256:
  `058de5ec6ca86484e0c41d28abd69e5ffa34067ef87a9682f99145a1fc2feaa4`
- Historical downstream identity anchor:
  `4433a3612c7c2e28c8a86e49c97e80178a586eac08ff1fe5ee0aa7a668aedf12`
- Fixed configuration: `SIMPLE_PINHOLE`, sequential overlap 4, maximum image
  dimension 1024, mapper seed 0, four workers, and no pose, GPS, orientation,
  focus, ARKit, depth, or other priors.

The two hashes use different historical manifest encodings; the benchmark
records both and recomputes the live identity before work.

## Balanced macOS results

Three repeats used a rotated order:

1. CPU, Metal SIFT, Metal matching, combined
2. Metal SIFT, Metal matching, combined, CPU
3. Metal matching, combined, CPU, Metal SIFT

All 12 runs completed and passed strict proof. Median values are shown below.

| Route | Extract s | Match s | Map/BA s | Export s | Total s | Total vs CPU | Points | Observations | Reprojection | Peak RSS bytes | Dispatches |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| CPU 4 | 59.219 | 13.881 | 49.473 | 0.837 | 124.679 | control | 45,799 | 176,698 | 1.20611 | 1,587,855,360 | none |
| Metal SIFT | 56.875 | 12.045 | 40.046 | 0.739 | 109.707 | -12.01% | 38,142 | 144,224 | 1.20597 | 775,962,624 | SIFT 115 |
| Metal matching | 59.154 | 40.632 | 51.652 | 0.842 | 152.034 | +21.94% | 46,668 | 180,686 | 1.21053 | 1,338,884,096 | matching 890 |
| Combined | 56.888 | 32.975 | 39.238 | 0.745 | 130.159 | +4.40% | 38,650 | 146,813 | 1.20559 | 601,817,088 | SIFT 115; matching 890 |

Every Metal row had zero SIFT and matching fallbacks and reported
`Apple M1 Pro`. Per-run timings, cold/warm order, requested/effective threads,
power/thermal snapshots, database byte and logical hashes, sparse-model hashes,
and output metrics are retained at:

- `dist/colmapkit-standard-metal-20260819-macos/experiment.json`
  (`df1e15296af0fdbc09dd291974b073eb2baa60895946fd4424b1eb2797e4961c`)
- `dist/colmapkit-standard-metal-20260819-macos/summary.csv`
  (`ff3b3c4b69f18cae0a2ba7f85aa3eeb17e35763607b81f338b8093ced2cc47ac`)

Exact benchmark command:

```bash
python3 scripts/python/colmapkit_standard_metal_benchmark.py \
  --colmapkit-bin build-colmapkit-v2-dev/src/colmap/colmapkit/colmapkit_sparse_reconstruct \
  --image-dir '/Users/brw/Documents/Splats Projects/Unsorted Scenes.splatsproject/scenes/control-native-1B9985D8/attempts/starter-20260712-190850/selected-frames' \
  --image-list '/Users/brw/Documents/Splats Projects/Unsorted Scenes.splatsproject/scenes/control-native-1B9985D8/attempts/starter-20260712-190850/manifests/colmap-image-list.txt' \
  --run-dir dist/colmapkit-standard-metal-20260819-macos \
  --repeats 3 --workers 4 --max-image-size 1024 \
  --sequential-overlap 4 --camera-model SIMPLE_PINHOLE \
  --mapper-random-seed 0 \
  --expected-manifest-sha256 058de5ec6ca86484e0c41d28abd69e5ffa34067ef87a9682f99145a1fc2feaa4
```

## Apple package

- XCFramework:
  `dist/colmapkit-standard-metal-20260819-candidate/ColmapKit.xcframework`
- Archive:
  `dist/colmapkit-standard-metal-20260819-candidate/ColmapKit.xcframework.zip`
- Archive bytes: `43,029,056`
- SHA-256 and SwiftPM checksum:
  `b1a4657786c9bd1cf49b2df7d299e6aa0bbf81142fd9ceb9ffb73256dc5b0d4b`
- Source identity embedded by the build:
  `42f24bd78ceb96b045f072d1b8c311556652497c`, clean patch SHA-256
  `e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855`
- Slices: macOS arm64 minimum 15.0; iOS arm64 minimum 18.0; iOS Simulator
  arm64 minimum 18.0.
- All slices have byte-identical public headers and module maps and the same 23
  expected ColmapKit exports.
- The package has no Homebrew, user-directory, or macOS-26 runtime dependency.
  macOS OpenMP is the framework-local vendored `libomp.dylib`.
- SIFT metallib hashes: macOS
  `a95d5ab393477f9c798733acdee28c3bb838554f0a3983423a0c10aa31137111`,
  iOS `5b3cd877813a5a05776fefa8844a4d540a5d994a9bbba25b0c82972344bb1a29`,
  Simulator
  `0599bb249c5b5148775416504b5bc56a70af5bfc7f1d1414fdc2163beb7e243d`.

Canonical package command:

```bash
COLMAPKIT_APPLE_BUILD_ROOT="$PWD/build-colmapkit-apple-sceneprep" \
COLMAPKIT_APPLE_DIST_ROOT=dist/colmapkit-standard-metal-20260819-candidate \
MACOS_DEPLOYMENT_TARGET=15.0 IOS_DEPLOYMENT_TARGET=18.0 \
SIFT_METAL_ENABLED=ON \
COLMAPKIT_VCPKG_ROOT=/private/tmp/colmap-vcpkg-upstream-20260818 \
bash scripts/build_colmapkit_apple_xcframework.sh
```

The pinned vcpkg checkout was at
`/private/tmp/colmap-vcpkg-upstream-20260818`, commit
`a0b1c8d3a477c1cb4813d8e127a56961707ca42b`.

Important package boundary: this is the repository's existing dynamically
linked framework topology with statically incorporated third-party libraries,
except the framework-local OpenMP dylib. It is not a literal static Mach-O
framework. Converting it would change resource and OpenMP integration semantics
and was not done speculatively. If “static XCFramework” is meant literally,
that remains a separate consumer/build-policy decision and this candidate does
not satisfy it.

## Compatibility and runtime proof

Focused C++ tests:

```bash
ctest --test-dir build-colmapkit-v2-dev \
  -R 'colmapkit|feature/(sift|extractor|metal_matcher)_test' \
  --output-on-failure
```

Result: 6/6 passed.

A client compiled from the published v0.2.1 header against this candidate
reported:

```text
version=COLMAP 4.2.0.dev0 (Commit 42f24bd7 on 2026-08-19 without GPU support) config_size=136 result_size=1088 status=1 guard=unchanged
```

The exact receipt is
`dist/colmapkit-standard-metal-20260819-candidate/audits/old-v0.2.1-header-client.log`.

Simulator command:

```bash
COLMAPKIT_XCFRAMEWORK_PATH="$PWD/dist/colmapkit-standard-metal-20260819-candidate/ColmapKit.xcframework" \
COLMAPKIT_RUNTIME_BUILD_ROOT="$PWD/build-colmapkit-standard-metal-simulator" \
COLMAPKIT_RUNTIME_RESULT_ROOT="$PWD/dist/colmapkit-standard-metal-20260819-candidate/simulator-runtime" \
bash scripts/run_colmapkit_ios_runtime_harness.sh
```

The Simulator remained nominal and passed legacy reconstruction,
post-processing, cancellation, tracked-pose V2, and RGB-prior V2. Strict SIFT
and strict matching completed with 8 and 34 dispatches respectively. The
combined route proved 8 + 34 dispatches and zero fallback, then failed at
mapping; it is preserved as a failed reconstruction receipt. Result SHA-256:
`265b4abd44698b5d2989fa40439c5c33f37a1dd206d1dcbaceaa467f29997484`.

Physical M1 iPad command:

```bash
COLMAPKIT_XCFRAMEWORK_PATH="$PWD/dist/colmapkit-standard-metal-20260819-candidate/ColmapKit.xcframework" \
COLMAPKIT_DEVICE_BUILD_ROOT="$PWD/build-colmapkit-standard-metal-device" \
COLMAPKIT_DEVICE_RESULT_ROOT="$PWD/dist/colmapkit-standard-metal-20260819-candidate/device-runtime" \
COLMAPKIT_DEVICE_ID=302F7720-91AC-5103-A977-592E006C1FF7 \
COLMAPKIT_DEVELOPMENT_TEAM=XKUXN9R3RW \
bash scripts/run_colmapkit_ios_device_runtime_harness.sh
```

Device: `iPad (2)`, M1 iPad Pro 11-inch (3rd generation), iPadOS 27.0 beta,
wired, unlocked, Developer Mode enabled. No host benchmark runner was active.
The harness refuses to start unless thermal state is nominal; it was nominal at
both start and end.

- Strict SIFT-only: 8 SIFT dispatches, zero fallback, `Apple M1 GPU`; later
  failed to create a sparse model and is not a successful Metal result.
- Strict matching-only: completed with 34 matching dispatches and zero fallback.
- Strict combined: completed with 8 SIFT and 34 matching dispatches and zero
  fallback.
- Legacy/post-processing/cancellation/tracked-pose/RGB-prior routes passed.

Result:
`dist/colmapkit-standard-metal-20260819-candidate/device-runtime/result.json`
with SHA-256
`1ab7562210bf04ecaa28c58823bdbc5aa0599e454580de78dc41dbb9a0694c30`.
This is a synthetic strict smoke, not a 115-image physical performance or
quality acceptance run. No repeated iPad performance campaign was run because
no macOS Metal candidate cleared the promotion boundary.

## Exact Splats local experiment contract

Use only an explicit local candidate override. Do not replace the published
v0.2.1 control or change the four-worker product policy.

1. Point the local candidate seam at the archive above and verify its exact
   SHA-256 before extraction.
2. Require the complete 168-byte config and 1,376-byte result before using any
   extended field. Set both `struct_size` values. Reject partial tails.
3. Keep the CPU control at four extraction, matching, and mapper workers with
   `use_metal_sift=0`, `use_metal_matching=0`, and both strict flags zero.
4. For SIFT-only, set `use_metal_sift=1` and `require_metal_sift=1`; keep Metal
   matching off. For matching-only, set `use_metal_matching=1` and
   `require_metal_matching=1`; keep Metal SIFT off. Combined sets both pairs.
5. Always provide a unique `evidence_path`. A route may be labeled Metal only
   when the call succeeds, `no_fallback_satisfied==1`, the requested effective
   backend is Metal, its dispatch count is positive, its fallback count is zero,
   and the evidence JSON agrees. Preserve later-stage failures as failure
   receipts; do not include them in speed comparisons.
6. Hold the exact image bytes/list, maximum dimension, camera model, matching
   topology, mapper settings, and output validation fixed. Do not attribute
   reduced matching/mapping work after Metal SIFT to stage acceleration.

Known limitations and remaining gates:

- no route meets the speed-and-quality promotion boundary;
- Metal SIFT changes feature/geometry workload materially on the representative
  fixture and logs bounded orientation-drop warnings;
- strict success on the tiny synthetic fixture varies at the mapper boundary,
  so backend proof and reconstruction success must remain separate fields;
- literal-static packaging, a 115-image physical benchmark, Splats integration,
  product quality review, and any release/pin remain unperformed and
  unauthorized.
