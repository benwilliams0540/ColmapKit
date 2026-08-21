# ColmapKit RC2 count-contract physical acceptance predeclaration

Status: predeclared before build-only or physical harness execution. This file
contains no device result and must not be edited to retrofit one.

## Preserved evidence and experiment boundary

RC1, physical attempts 1 through 3, the 256 MiB rejection, the 320 MiB
count-contract rejection, and the macOS cross-slice diagnostic remain
unchanged. RC2 tests one general owner repair only: `max_num_features` is the
inclusive hard limit on terminal oriented keypoint/descriptor rows. The
extractor deterministically retains the largest-scale aligned rows when raw
orientation expansion exceeds that limit. No image, feature profile, quality
threshold, backend, worker, import, lifecycle, resource, or thermal dimension
changes.

## Immutable RC2 package

- Owner package source:
  `670a14bb71c5d543769251e608bcfacbef85fdd4`
- Runtime: `0.3.0-rc.2+670a14bb`
- Framework version/build: `0.3.0` / `2`
- ZIP path:
  `dist/colmapkit-v0.3.0-rc2-670a14bb/ColmapKit.xcframework.zip`
- ZIP bytes: `43213631`
- ZIP SHA-256 / SwiftPM checksum:
  `2ff308666327193012cb9b6d2974d3d58ecb3e1b2714e885bdb52c3e8cf2bf70`
- iPhoneOS binary SHA-256:
  `7e4dbc6709d0b698e4dcc783c302c87534e2b2f81f8cb43ee45463b18f2cc4ed`
- Public header SHA-256:
  `73fe2af23e3cb2811add36ffb65ea020330f209788a743296a33eb18e5974d25`
- Module map SHA-256:
  `d0af903600e0e3dcc4846ace8a88fcf64698daa36b489ce9a41aedfa60a0b9be`
- Consolidated notices SHA-256:
  `82b6522d3fba08354eff2d3c5dc77266c4c0d30b406374ab45b208f5f229d0a4`
- Source patch SHA-256:
  `e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855`

The package has macOS arm64 minimum 15.0, iPhoneOS arm64 minimum 18.0, and
iOS Simulator arm64 minimum 18.0 slices; identical headers, module maps, and
34-symbol export manifests; profiler and Metal SIFT disabled; iOS system-only
linkage; and framework-local OpenMP on macOS. Fresh external SwiftPM builds
passed for all three destinations. Independent archive extraction preserved
all 24 packaged file hashes. A released-v0.2.1-header client preserved the
136/1088-byte sparse layouts and bounded writes. Two packaged macOS runs each
emitted and validated 4096 rows, reported 325534076 admitted bytes, and
produced identical artifact SHA-256
`f9eaaaa2f6cac8dae923d47bd6fb782ad835f20c8bb5abf0227675f8dc70a9ec`.

## Device and fixture identity

The only accepted device is the connected wired, unlocked M1 iPad:

- CoreDevice identifier: `302F7720-91AC-5103-A977-592E006C1FF7`
- UDID: `00008103-0012086A1179001E`
- product/model: `iPad13,6`, iPad Pro (11-inch) (3rd generation)
- OS: iPadOS 27.0, build `24A5370h`
- CPU: arm64e, eight cores
- internal storage capacity: 128000000000 bytes

The run must recheck wired/paired/booted/unlocked state, at least 2 GiB free
storage, at least 20% battery, charge and Low Power Mode, nominal/fair thermal,
and absence of a competing host or device benchmark immediately before launch.

Fixture manifest SHA-256:
`cb1ac79e7592c3bb33903e62ee342596597752f48f7b9f64582ca5102c639e1a`.
The three JPEG SHA-256 identities are:

1. `2b22ced55baccd6493e49d8b5086367c34c952ec63399ea5e96c3dd52ace1767`
2. `45c47baa3b4242576b694ce292eb8d9eef38398a12b8f5ed31a559f8371d156a`
3. `83067fdd42d5eda3e9592b28677770a228f52b7207f8a9b055c4ae6442447c77`

The unique retained attempt root is:
`dist/colmapkit-v0.3.0-rc2-670a14bb/physical/attempt-01-count-contract-full-matrix`.

## Frozen numeric profile and admission rule

- CPU backend, one worker, no fallback, no Metal combination.
- Maximum encoded JPEG bytes: 16777216.
- Maximum image dimension: 1024.
- `max_num_features`: 4096 terminal oriented rows.
- First octave -1; four octaves; octave resolution 3.
- At most two orientation hypotheses per localized keypoint; `upright=false`.
- L1-root normalization; peak threshold `0.006666666666666667`; edge
  threshold `10.0`.
- Import CREATE_NEW, one worker, 64 MiB artifact/image/base-database bounds,
  and 100000 total features.

For each immutable frame, admission is:

```text
2 * encoded_bytes
+ ceil(scaled_width) * ceil(scaled_height) * first_octave_factor * 96
+ max_num_features * 256
+ 16 MiB
```

The maximum repaired estimate is exactly 325534076 bytes. Rounding that value
up once to the next 16 MiB boundary selects the unchanged 335544320-byte
(320 MiB) budget. There is no budget sweep or fallback.

## Exact operation sequence

Use only the bounded owner harness. Record package/app/device boundaries;
extract and validate the representative frame twice; compare exact bytes;
cancel after the first truthful safe boundary and wait terminally; extract the
ordered three-frame set; transactionally import twice to fresh destinations;
inspect cameras, images, keypoints, descriptors, IDs, match absence, receipts,
and database identities; reject corrupt, truncated, stale, preexisting,
duplicate, partial, mixed-profile, and destination-conflict cases; reject an
explicit Metal request without CPU fallback; execute ten sequential extraction
and two import lifecycles; and sample RSS, thermal, battery, charge, and Low
Power Mode every five seconds plus both boundaries.

## Retain/reject gates

All RC1 gates remain unchanged except the corrected public count assertion:

- exact RC2 source, ZIP, slice, runtime, fixture, and profile identities;
- actual CPU backend, one effective worker, and no fallback;
- every artifact has aligned finite six-column keypoints, 128 uint8 descriptor
  bytes per row, and `1 <= feature_count <= 4096`;
- the representative high-texture frame reaches exactly 4096 terminal rows,
  proving the repaired hard bound at the formerly inconsistent fixture;
- same-device repeat artifact bytes and repeat import receipt/database/sealed
  identities are equal (not a cross-device determinism claim);
- cancellation reaches terminal CANCELLED within five seconds and leaves no
  authoritative or temporary output;
- both imports contain exactly the declared rows with deterministic camera and
  image IDs and no match/two-view rows;
- every invalid/conflict/partial case fails closed and preserves foreign data;
- strict Metal request rejects before feature work with no CPU fallback;
- ten extraction and two import lifecycles finish; engine peak RSS is below
  768 MiB; terminal RSS growth is at most 128 MiB;
- start storage is at least 2 GiB and battery at least 20%; thermal never
  reaches critical and ends no worse than serious; all five-second and boundary
  resource samples are present.

Any identity, correctness, transaction, no-fallback, cancellation, lifecycle,
resource, thermal, or evidence failure rejects RC2 and stops the goal. No
second source fix, budget, threshold, fixture, or product-policy change is
authorized. Passing authorizes only the separate final-local-v0.3.0 goal, not
tag, push, release, publication, public SwiftPM update, or consumer pin.
