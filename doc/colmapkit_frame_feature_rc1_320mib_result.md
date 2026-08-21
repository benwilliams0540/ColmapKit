# ColmapKit RC1 320 MiB physical follow-up result

## Verdict

**Rejected. Do not build final v0.3.0, release, publish, or pin RC1.** The one
predeclared 320 MiB candidate passed its deterministic admission check and the
exact RC1 CPU extractor completed the first frame. The harness then rejected
the completed result at the frozen feature-count gate. A same-input diagnostic
against the exact RC1 macOS slice localized the owning defect: extraction emits
8,736 oriented features, while the artifact parser rejects anything above
`4096 * 2 = 8192`. The package therefore cannot validate or import its own
artifact for this frozen input/profile.

The complete physical matrix stopped at that first invalid boundary. No second
budget, sweep, resolution/feature reduction, threshold, fallback, algorithm
change, final-version build, or follow-up physical run occurred.

## Authority and identities

- Owner branch: `integrate/upstream-main-2026-08`
- Harness/config commit:
  `7cf1daa72a7106c08883a39fba0645b215c7a51a`
- Predeclaration commit:
  `72be77967e6406a4a1a655d54968045916260a11`
- Predeclaration SHA-256:
  `feafda1455d65f6423485d7064d16b005a353535ea0561d6bf5fc0dd3d7c8338`
- Exact RC source: `c69711b89848a7ffe0b8933ea8636f87be8c1c50`
- Runtime: `0.3.0-rc.1+c69711b8`
- ZIP SHA-256 / SwiftPM checksum:
  `e7e69b029715bfe4f63551c05fdc9851ef565844f216669b5e0fbea5ba8a5aa3`
- iPhoneOS binary SHA-256:
  `8876954755d656e1968426d411c24ac48e87d1903ee5f449126f042f6a704eb0`
- Base matrix SHA-256:
  `47071bd7ef9a2a94dab5b991faec97e5f998646049514c1cfd3582262c6f086d`
- Fixture manifest SHA-256:
  `cb1ac79e7592c3bb33903e62ee342596597752f48f7b9f64582ca5102c639e1a`
- Only changed request field: `memory_admission_budget_bytes = 335544320`.

The exact device was the wired, paired, unlocked physical M1 iPad `iPad13,6`,
CoreDevice identifier `302F7720-91AC-5103-A977-592E006C1FF7`, iPadOS 27.0
build `24A5370h`. No competing ColmapKit, BrushKit, Splats, or benchmark
process was present before launch or after termination.

## Physical boundary and failure

The retained physical attempt is:

`/Users/brw/Developer/ai-projects/colmap/dist/colmapkit-v0.3.0-rc1-c69711b8/physical/attempt-03-320mib-full-matrix`

Preflight reported:

- exact engine source
  `COLMAP 4.2.0.dev0 (Commit c69711b8 on 2026-08-20 without GPU support)`;
- requested backend `1` (CPU), one worker, no Metal combination;
- 335,544,320-byte budget and 326,582,652-byte maximum fixture estimate;
- 11,007,327,160 available bytes;
- 25% battery, unplugged, Low Power Mode false;
- nominal thermal state;
- 56,770,560 process resident bytes.

The first CPU job completed rather than failing admission. The harness terminal
message was:

```text
primary-10 extraction failed: CPU frame feature artifact completed.
```

At that boundary the device remained nominal, 25% and unplugged with Low Power
Mode false, reported 11,007,249,336 available bytes, and had 310,345,728
resident bytes. These are process boundary samples, not the engine-returned
peak-RSS field. The failure-path receipt did not preserve the completed C result
fields; that remains a harness telemetry limitation. The result is nonetheless
a matrix rejection because exact validation, cancellation, import, invalid-case
coverage, repeat lifecycle, and final resource gates were not reached.

## Owning defect

The local diagnostic used the exact RC1 macOS slice, the same frame bytes and
metadata, and the same frozen configuration. It repeated identically and
reported:

```text
status=OK backend=CPU no_fallback=1 workers=1
features=8736 descriptor_bytes=1118208 admitted=326582652
profile=ab82592417f33d53887fd9e641eee2cb1cb92d7727cc5266e2decdc5ac1d669f
source=COLMAP 4.2.0.dev0 (Commit c69711b8 on 2026-08-20 without GPU support)
validate=INVALID_ARGUMENT message=Invalid frame feature counts.
```

This is an owner API/artifact-contract defect, not a remaining budget or
harness-only issue:

1. CPU SIFT's `max_num_features` selection retains an entire DoG level after
   crossing the requested base-feature limit, so it can exceed 4,096 base
   keypoints.
2. Orientation expansion produces 8,736 rows for this fixture.
3. `FrameFeatureExtractionV1` serializes those rows without enforcing its
   artifact parser's bound.
4. The parser requires `feature_count <= max_num_features *
   max_num_orientations`, which is 8,192, and rejects the artifact the same
   operation just emitted.
5. The admission estimate also budgets feature storage using the same 8,192
   bound, although actual oriented rows exceed it.

The frozen matrix's stricter `1...4096` gate also fails, but relaxing that
harness assertion would not make RC1 valid: its owner validator still rejects
8,736. A future source goal must define and test one deterministic bounded
feature-row contract shared by extraction, memory admission, serialization,
validation, and import. That work is unstarted and cannot reuse RC1 bytes.

## Evidence hashes

- `build-receipt.txt`:
  `cc1d0ed4cdec11cb19c636582e4b1490c21cb7b491a69c5dce9da9b3e68ffb0f`
- `device-details.txt`:
  `1b31add049adab83f820d4451892c0fc631acee5a325f7a75aa52fc3be052da5`
- `device-lock-state.txt` and `device-lock-state-after.txt`:
  `f4cac25f8252ae5c027b8724c18882f27f28480a68825933842ac9745ab5692e`
- `device-processes-before.txt` and `device-processes-after.txt`:
  `aae3ddf7d57d7f7c68fb59698c15be8264518919f8d402ce72f242ee727be1d9`
- `install.log`:
  `bbd182ed53f792a977a2ff75f337f4dcfe0e106c19fbcef14371bb2318b966d8`
- `runtime.log`:
  `df7416b51cdc6658e8e2f405009bab8c1eebde51c3591acf6a6da541860b8e45`
- `preflight.json` / `preflight.pretty.json`:
  `c408973e099ec06408f594b909f883b7625a79178e48fafdbcf8b9d732329ecb` /
  `1a969eb4e53795937997f14f37ac1be61b99d44ec2be120bcf8e97d58b8f1f81`
- `failure.json` / `failure.pretty.json`:
  `1c6e719c0fb076c82822be98a8645e952b8913bf57aad832955390775aff93b9` /
  `c58cb9f63e00674d6a5265372ee3d85fa29af4986bcf31b3eea6a7f4cc5605e4`
- `xcodebuild.log`:
  `841e2213c70c9a8ea9b2447e7c6e9c15a34847e815f7688a72dfb227c5034a11`
- `mac-result-probe.cc` / `mac-result-probe.log`:
  `5a99d972ab881c6858d8c20821dcb00fdd3cbf67ad0f0649bae33dece0454f32` /
  `3b92c0f08845738e8aef19fd7f49e53f027a4d6e9f32abcccded13d8a57fd08a`.

No Splats/BrushKit/msplat edit or run, push, PR, remote CI, tag, release,
publication, public SwiftPM update, consumer pin, product-default change, or
manual-quality claim occurred.
