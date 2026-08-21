# ColmapKit v0.3.0 exact-final physical acceptance predeclaration

Status: predeclared after RC3 passed its immutable physical matrix and after
the exact final package passed its three-slice static audit, but before any
final-package device query, harness build, install, or launch. This file
contains no final-package device result and must not be edited to retrofit one.

## Accepted RC3 boundary

The preserved RC3 source `14eadc5cad82eb7a53b52d8739b4345054f15d46`,
runtime `0.3.0-rc.3+14eadc5c`, and ZIP SHA-256
`fe7c88966b0f4f3e67d003a0d0160d40a716590db769a465f732ffbc31d48531`
passed the unchanged physical acceptance matrix on the exact M1 iPad. The
retained attempt is
`dist/colmapkit-v0.3.0-rc3-14eadc5c/physical/attempt-02-authorized-full-matrix-20260821`;
its `result.json` SHA-256 is
`2fbcd0c2d9ed1160805e5ad911d0ca52ceb5cac84e1b9cc2ba5be12ec054b629`.

The final package source differs from RC3 only through committed owner
documentation and bounded device/readiness harness work. There is no change to
the ColmapKit engine, public header, extraction or import implementation,
feature profile, admission estimator, fixture, acceptance threshold, or
fallback policy. Because the final package embeds a different version and
source identity and therefore has different bytes, this declaration repeats
every substantive RC3 physical gate against those exact final bytes.

## Immutable final local package

- owner package source: `bb91933693e1a1d26e43ccc2358e3dcc2e702caa`
- runtime: `0.3.0`
- framework version/build: `0.3.0` / `4`
- ZIP: `dist/colmapkit-v0.3.0-final-bb919336/ColmapKit.xcframework.zip`
- ZIP bytes: `43213354`
- ZIP SHA-256 / SwiftPM checksum:
  `ec42eefbcb91d9741d7a7cfd5f4e4d8cc35fd9dbe7991733d3ded2656320caf3`
- iPhoneOS binary SHA-256:
  `45aa61ab599f0e9b0e04a15ffe6a2faeb15e9e024ebb95d15c315195caa05242`
- macOS binary SHA-256:
  `2cc3e035386adbf3f0ae704af00d5e29caff22fc1be349622b0fbbac92908a36`
- iOS Simulator binary SHA-256:
  `01d9b0edebcc1f4a59b12cdfecbaa7a514a1b4d32f43d37ab4a25a2b18396e8b`
- public header SHA-256:
  `73fe2af23e3cb2811add36ffb65ea020330f209788a743296a33eb18e5974d25`
- module map SHA-256:
  `d0af903600e0e3dcc4846ace8a88fcf64698daa36b489ce9a41aedfa60a0b9be`
- consolidated notices SHA-256:
  `82b6522d3fba08354eff2d3c5dc77266c4c0d30b406374ab45b208f5f229d0a4`
- source patch SHA-256:
  `e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855`

The package contains independently audited macOS arm64 minimum 15.0,
iPhoneOS arm64 minimum 18.0, and iOS Simulator arm64 minimum 18.0 slices.
Headers, module maps, and 34-symbol export manifests are identical. Profiling
and Metal SIFT are disabled; iOS has system-only linkage; macOS uses only the
framework-local OpenMP library. All three fresh external SwiftPM consumer
builds, package signature/property-list/license audits, focused extraction and
import tests, and the released-v0.2.1-header client passed before this device
execution. The old-header client observed the unchanged 136/1088-byte sparse
layouts and bounded output guard.

The device harness identity gate is parameterized by committed owner test
change `97defc36df829a3098d3fad9008ef7c78a51d8db`. Its unchanged defaults still
target immutable RC3; this execution explicitly supplies the final release,
source, ZIP, matrix, and iPhoneOS framework identities above.

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
The device model and UDID must not change.

Fixture manifest SHA-256:
`cb1ac79e7592c3bb33903e62ee342596597752f48f7b9f64582ca5102c639e1a`.
The three JPEG SHA-256 identities are:

1. `2b22ced55baccd6493e49d8b5086367c34c952ec63399ea5e96c3dd52ace1767`
2. `45c47baa3b4242576b694ce292eb8d9eef38398a12b8f5ed31a559f8371d156a`
3. `83067fdd42d5eda3e9592b28677770a228f52b7207f8a9b055c4ae6442447c77`

The unique retained attempt root is
`dist/colmapkit-v0.3.0-final-bb919336/physical/attempt-01-exact-final-v030-20260821`.

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

- exact final source, ZIP, slice, runtime, fixture, and profile identities;
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
resource, thermal, or evidence failure rejects the final local candidate and
stops. No rebuild, source fix, budget, threshold, fixture, retry, sleep/GC
workaround, or product-policy change is authorized by this matrix. Passing is
only evidence for Ben's separate release decision; it does not authorize a
tag, push, release, publication, public SwiftPM update, or consumer pin.
