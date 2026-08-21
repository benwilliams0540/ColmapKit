# ColmapKit v0.3.0 RC1 physical acceptance rejection

## Verdict

The local RC1 package is **rejected by its predeclared physical matrix**. Do
not release, publish, or pin it. The exact package reached the connected M1
iPad, but the first `FrameFeatureExtractionV1` request failed before feature
work because its declared 256 MiB admission budget was smaller than the
engine's deterministic estimate for the frozen 1024-by-768, 4096-feature,
two-orientation CPU profile. No `.ckfeatures` artifact, import, cancellation,
lifecycle, or quality claim was produced. No final `v0.3.0` rebuild was
started.

This result preserves the predeclared budget and matrix unchanged. It is not a
budget sweep and must not be reinterpreted as physical acceptance.

## Immutable identities

- Package source: `c69711b89848a7ffe0b8933ea8636f87be8c1c50`
- Owner branch at execution: `integrate/upstream-main-2026-08`
- Owner HEAD before the harness-record commit: `f0e45c88ff4deda64a513bd257f806786b9c8f5e`
- Runtime: `0.3.0-rc.1+c69711b8`
- ZIP: `/Users/brw/Developer/ai-projects/colmap/dist/colmapkit-v0.3.0-rc1-c69711b8/ColmapKit.xcframework.zip`
- ZIP bytes: `43212429`
- ZIP SHA-256 / SwiftPM checksum:
  `e7e69b029715bfe4f63551c05fdc9851ef565844f216669b5e0fbea5ba8a5aa3`
- iPhoneOS packaged binary SHA-256:
  `8876954755d656e1968426d411c24ac48e87d1903ee5f449126f042f6a704eb0`
- Matrix SHA-256:
  `47071bd7ef9a2a94dab5b991faec97e5f998646049514c1cfd3582262c6f086d`
- Fixture manifest SHA-256:
  `cb1ac79e7592c3bb33903e62ee342596597752f48f7b9f64582ca5102c639e1a`
- Public header / module map / notices SHA-256:
  `41fd5cfea9c168fd33ab7443ffbbfb9d1007e550cd04ea4c7d2b8908778aab1f`,
  `d0af903600e0e3dcc4846ace8a88fcf64698daa36b489ce9a41aedfa60a0b9be`,
  and `82b6522d3fba08354eff2d3c5dc77266c4c0d30b406374ab45b208f5f229d0a4`.

The three-slice static audit and external SwiftPM consumer builds remain green
as recorded in `doc/colmapkit_frame_feature_package_readiness.md`. Physical
rejection supersedes that static evidence for release readiness.

## Device and execution boundary

The target was the wired, paired, booted, developer-enabled, unlocked physical
`iPad13,6` (iPad Pro 11-inch, 3rd generation, M1), CoreDevice identifier
`302F7720-91AC-5103-A977-592E006C1FF7`, device UDID
`00008103-0012086A1179001E`, on iPadOS 27.0 build `24A5370h`. The device reports
128,000,000,000 bytes of total internal storage. The preflight process list
contained no ColmapKit, BrushKit, Splats, or benchmark process, and the owner
harness was absent after termination.

The harness evaluated its boundary guards before starting extraction. That
proves available storage was at least 2,147,483,648 bytes, battery was at least
20%, and thermal state was either nominal or fair. Exact available-storage,
battery, thermal-category, charge, Low Power Mode, and RSS values were not
emitted on the early error path and cannot be reconstructed from the retained
logs. This is a harness failure-receipt gap; it must be corrected before any
separately authorized follow-up, without changing this rejected result.

## Admission calculation and ownership

For frame 10, the first and largest encoded fixture (`2,859,198` bytes), the
engine calculates:

```text
scale = min(1, max_image_size / max(encoded_width, encoded_height)) = 1
pixels = ceil(1024 * scale) * ceil(768 * scale) = 786,432
first_octave_factor = 4 because first_octave = -1
encoded copies = 2 * 2,859,198 = 5,718,396
image working set = 786,432 * 4 * 96 = 301,989,888
feature working set = 4,096 * 2 * 256 = 2,097,152
fixed allowance = 16 * 1,048,576 = 16,777,216
estimated admission = 326,582,652 bytes = 311.453487396 MiB
declared budget = 268,435,456 bytes = 256 MiB
excess = 58,147,196 bytes = 55.453487396 MiB
```

This is conservative deterministic admission accounting, not measured or
predicted physical RSS. It bounds decoded/SIFT working storage, feature
storage, two encoded-image copies, and a fixed allowance. The request was
rejected synchronously before a feature worker or backend dispatch began, so
no physical peak-RSS measurement exists for this attempt.

The rejection is configuration/profile-owned: the frozen matrix combined a
1024/4096/two-orientation/first-octave-minus-one request with a budget below
the engine's published calculation. The harness forwarded that matrix
faithfully, and the API failed closed as designed. This does **not** expose a
ColmapKit admission API defect. Separately, attempt 1 exposed a harness identity
check that compared a required developer-signed Mach-O byte-for-byte with the
ad-hoc packaged Mach-O; the one authorized correction now requires equal
signature-normalized executable bytes while preserving both raw hashes.

The smallest general rule for one future, separately authorized one-variable
candidate is: evaluate the existing admission formula for every immutable
frame using its actual encoded bytes and frozen metadata/configuration, take
the maximum, and round once to the next 16 MiB boundary. For this exact fixture
that predeclares **320 MiB (335,544,320 bytes)**, with no sweep and no other
profile change. The same exact RC1 ZIP remains technically eligible for that
caller-configuration-only follow-up because source, package, backend, and
algorithms need not change. This record does not authorize or start it.

## Preserved attempts

Attempt 1 stopped before install because developer signing necessarily changed
the raw framework Mach-O hash from
`8876954755d656e1968426d411c24ac48e87d1903ee5f449126f042f6a704eb0` to
`24295c89bbf66fc2521e045e5c286b9f4b6a7f7aa302d1e238307b0a62230d25`.
Removing only the signature envelope from temporary copies produced identical
SHA-256
`8378825099065e75ded0f515b451b7b45434bd9101da0b141ba245aaef923451`.
The retained directory is:

`/Users/brw/Developer/ai-projects/colmap/dist/colmapkit-v0.3.0-rc1-c69711b8/physical/attempt-01-exact-byte-guard`

- `device-details.txt`: `396e4c2f34f445ec049efffea690d6f3b5f31f48d4f4db38c2dbb4beb87ea535`
- `device-lock-state.txt`: `f4cac25f8252ae5c027b8724c18882f27f28480a68825933842ac9745ab5692e`
- `device-processes-before.txt`: `ef8667272d77d9a21ea29281b8340215f5210db34861f43d5bdb3fd4c5b207df`
- `failure-receipt.txt`: `55a015fbd889d62d9bd26400b48bbae155084cf0110bdc754fb412ba5421876a`
- `xcodebuild.log`: `4bc393defe478809936494f33f1349e9e05da87a0aafc47eb9673f359c911327`

Attempt 2 built, installed, launched, and terminated at the admission gate. Its
retained directory is:

`/Users/brw/Developer/ai-projects/colmap/dist/colmapkit-v0.3.0-rc1-c69711b8/physical/attempt-02-signature-normalized`

- `build-receipt.txt`: `2089059be84c32c78158a25cc099bc0948eb5daa1914ece8c06e587fabdda5dc`
- `device-details.txt`: `f668770aae03f990cc325aee93eab98c6f701058ad5d03d78706c8e879a96279`
- `device-lock-state.txt`: `f4cac25f8252ae5c027b8724c18882f27f28480a68825933842ac9745ab5692e`
- `device-processes-before.txt`: `210209561e4dea8493d5c47909d86f999dc940c08d755a3d65793d13888e485f`
- `install.log`: `f09bff507a6aaff1799eaec9405d73c3e4b605383dcb0d1565e8be4942e2761b`
- `runtime.log`: `3e2f427e32a4840b99b19464329d31c24d0040a9c445f9493be7f82001ed9cd7`
- `xcodebuild.log`: `42c551f3902a5955df98a92584ddea7c931124548951ba59440605f3141f699c`

The runtime terminal message was:

```text
primary-10 start failed: Frame feature job exceeds the configured memory admission budget.
```

No device rerun, Splats install/run, algorithm or threshold change, package
rebuild, final-version goal, push, PR, CI, tag, release, publication, public
SwiftPM update, or consumer pin followed this rejection.
