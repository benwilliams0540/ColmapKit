# ColmapKit RC3 lifecycle physical acceptance predeclaration

Status: predeclared after the exact RC3 static package audit and before any RC3
device query, harness build, install, or launch. This file contains no device
result and must not be edited to retrofit one.

## Preserved evidence and single repair boundary

RC1, RC2, physical attempts 1 through 3, both admission-budget records, the
RC2 count-contract repair, and the RC2 terminal-RSS rejection remain
authoritative and unchanged. RC3 tests exactly one source-owned lifecycle
repair: on Apple platforms, the four fixed VLFeat SIFT image-workspace buffers
use private anonymous mappings and are unmapped when the filter is destroyed.
Non-Apple allocation remains unchanged. The repair does not change extraction
math, feature counts, ordering, descriptors, defaults, ABI, schema, admission
accounting, or the 128 MiB terminal-growth gate.

The exact-source macOS diagnostic is retained at
`build/colmapkit-frame-feature-rss-diagnostic-14eadc5c-exact`. Two sequential
CPU/one-worker/no-fallback runs each returned 4096 rows and 524288 descriptor
bytes, reported 325534076 admitted bytes, and emitted identical artifact
SHA-256 `50f1282ffa84df854eab2f6099e543442dc3f7d4171dc93b7cfa11114572d331`.
The 622592 normalized keypoint/descriptor bytes are byte-identical to RC2.
Terminal RSS grew 27656192 bytes from process start, and the second completed
run added 98304 bytes at the terminal sample; both are below the unchanged
gates.

## Immutable RC3 package

- owner package source: `14eadc5cad82eb7a53b52d8739b4345054f15d46`
- runtime: `0.3.0-rc.3+14eadc5c`
- framework version/build: `0.3.0` / `3`
- ZIP: `dist/colmapkit-v0.3.0-rc3-14eadc5c/ColmapKit.xcframework.zip`
- ZIP bytes: `43213395`
- ZIP SHA-256 / SwiftPM checksum:
  `fe7c88966b0f4f3e67d003a0d0160d40a716590db769a465f732ffbc31d48531`
- iPhoneOS binary SHA-256:
  `1c34a7cc27a35c0004c6846dcfdc7c632c1fae60fe77a5ec6b95a72b089695d7`
- public header SHA-256:
  `73fe2af23e3cb2811add36ffb65ea020330f209788a743296a33eb18e5974d25`
- module map SHA-256:
  `d0af903600e0e3dcc4846ace8a88fcf64698daa36b489ce9a41aedfa60a0b9be`
- consolidated notices SHA-256:
  `82b6522d3fba08354eff2d3c5dc77266c4c0d30b406374ab45b208f5f229d0a4`
- source patch SHA-256:
  `e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855`

The independently audited package contains macOS arm64 minimum 15.0,
iPhoneOS arm64 minimum 18.0, and iOS Simulator arm64 minimum 18.0 slices;
identical headers, module maps, and expected export manifests; profiler and
Metal SIFT disabled; iOS system-only linkage; and framework-local OpenMP on
macOS. Fresh external SwiftPM consumer builds passed for all three
destinations. The first in-sandbox SwiftPM attempt was rejected because Swift
could not write its standard module cache; the exact audit was repeated
outside that filesystem restriction and passed without source or package
changes.

## Device and fixture identity

The only accepted device is the connected wired, unlocked M1 iPad:

- CoreDevice identifier: `302F7720-91AC-5103-A977-592E006C1FF7`
- UDID: `00008103-0012086A1179001E`
- product/model: `iPad13,6`, iPad Pro (11-inch) (3rd generation)
- expected OS boundary: iPadOS 27.0 build `24A5370h`
- CPU: arm64e, eight cores
- internal storage capacity: 128000000000 bytes

Immediately before launch, recheck wired/paired/booted/unlocked state, at
least 2 GiB free storage, at least 20% battery, charge and Low Power Mode,
nominal/fair thermal, and absence of a competing host or device benchmark.
Any device or OS identity drift is recorded and evaluated before execution;
the device model/UDID must not change.

Fixture manifest SHA-256:
`cb1ac79e7592c3bb33903e62ee342596597752f48f7b9f64582ca5102c639e1a`.
The three JPEG SHA-256 identities are:

1. `2b22ced55baccd6493e49d8b5086367c34c952ec63399ea5e96c3dd52ace1767`
2. `45c47baa3b4242576b694ce292eb8d9eef38398a12b8f5ed31a559f8371d156a`
3. `83067fdd42d5eda3e9592b28677770a228f52b7207f8a9b055c4ae6442447c77`

The unique retained attempt root is
`dist/colmapkit-v0.3.0-rc3-14eadc5c/physical/attempt-01-lifecycle-full-matrix`.

## Frozen profile and exact operation sequence

- CPU backend, one worker, no fallback, no Metal combination.
- Maximum encoded JPEG bytes 16777216; maximum image dimension 1024.
- `max_num_features=4096` terminal oriented rows.
- First octave -1; four octaves; octave resolution 3.
- At most two orientations; `upright=false`.
- L1-root normalization; peak threshold `0.006666666666666667`; edge
  threshold `10.0`.
- Existing deterministic estimate maximum 325534076 bytes, rounded once to
  the unchanged 335544320-byte admission budget.
- Import CREATE_NEW, one worker, 64 MiB artifact/image/base-database bounds,
  and 100000 total features.

Use only the bounded owner harness. Record exact package/app/device identities;
extract and validate the representative frame twice; compare exact bytes;
cancel after the first truthful safe boundary and wait terminally; extract the
ordered three-frame set; transactionally import twice to fresh destinations;
inspect camera/image IDs, keypoints, descriptors, receipts, databases, and
absence of match rows; reject corrupt, truncated, stale, preexisting,
duplicate, partial, mixed-profile, and destination-conflict cases; reject an
explicit Metal request without CPU fallback; execute ten sequential extraction
and two import lifecycles; and sample RSS, thermal, battery, charge, and Low
Power Mode every five seconds plus both boundaries.

## Unchanged retain/reject gates

- exact RC3 source, ZIP, slice, runtime, fixture, and profile identities;
- actual CPU backend, one worker, and no fallback;
- aligned finite six-column keypoints and 128 uint8 descriptor bytes per row;
- representative frame exactly 4096 terminal rows;
- same-device repeat artifact and import/database/sealed identities equal;
- cancellation terminal within five seconds with no authoritative/temp output;
- both imports contain exactly declared rows, deterministic IDs, and no
  match/two-view rows;
- every invalid/conflict/partial case fails closed and preserves foreign data;
- strict Metal request rejects before feature work with no CPU fallback;
- ten extraction and two import lifecycles finish;
- engine peak RSS below 768 MiB and terminal RSS growth at most 128 MiB;
- start storage at least 2 GiB and battery at least 20%; thermal never critical
  and ends no worse than serious; all five-second/boundary samples present.

Any identity, correctness, transaction, no-fallback, cancellation, lifecycle,
resource, thermal, or evidence failure rejects RC3 and stops. No second source
fix, budget, threshold, fixture, sleep/GC workaround, or product-policy change
is authorized. Passing authorizes only the separate final-local-v0.3.0 goal,
not a tag, push, release, publication, public SwiftPM update, or consumer pin.
