# ColmapKit FrameFeatureExtractionV1 RC2 result

Status: **rejected at the predeclared physical terminal-RSS gate**. RC2 must
not be released, published, pinned, or used to start the final-v0.3.0 goal.

## Source and repair

- Branch: `integrate/upstream-main-2026-08`
- Repair commit:
  `670a14bb71c5d543769251e608bcfacbef85fdd4`
- Physical-matrix/harness commit:
  `df29a0b73e0f6d16d0ccd45eea29133eebaf68dc`
- Predeclaration commit: `2a8bb0eab`

The repair makes `max_num_features` the inclusive hard maximum on terminal
oriented keypoint/descriptor rows for the unpublished frame-feature API.
Raw CPU SIFT may still generate extra orientation rows. Before result
reporting, serialization, validation, or import, aligned rows are selected
deterministically by the existing largest-scale ordering. The artifact parser
rejects counts above `max_num_features`, and the estimator's terminal-row term
is `max_num_features * 256` rather than
`max_num_features * max_num_orientations * 256`. No public structure, size,
prefix, schema version, or export changed.

Focused boundary coverage includes exact-bound preservation, deterministic
over-bound aligned selection, a high-texture/two-orientation extraction that
raw-exceeds and terminally reaches 4096 rows, repeat artifact identity,
one-byte-below/exact admission behavior, forged over-bound rejection, exact
imported row agreement, and pretransaction import rejection of an over-bound
artifact. The five focused ColmapKit tests passed 5/5 at repair source.

## Immutable RC2 package and static result

- Runtime: `0.3.0-rc.2+670a14bb`
- Directory:
  `/Users/brw/Developer/ai-projects/colmap/dist/colmapkit-v0.3.0-rc2-670a14bb`
- ZIP bytes: `43213631`
- ZIP SHA-256 / SwiftPM checksum:
  `2ff308666327193012cb9b6d2974d3d58ecb3e1b2714e885bdb52c3e8cf2bf70`
- iPhoneOS binary SHA-256:
  `7e4dbc6709d0b698e4dcc783c302c87534e2b2f81f8cb43ee45463b18f2cc4ed`
- Header / module map / notices SHA-256:
  `73fe2af23e3cb2811add36ffb65ea020330f209788a743296a33eb18e5974d25` /
  `d0af903600e0e3dcc4846ace8a88fcf64698daa36b489ce9a41aedfa60a0b9be` /
  `82b6522d3fba08354eff2d3c5dc77266c4c0d30b406374ab45b208f5f229d0a4`

The clean package has macOS arm64 minimum 15.0, iPhoneOS arm64 minimum 18.0,
and iOS Simulator arm64 minimum 18.0 slices; identical headers, module maps,
and 34-symbol exports; profiler and Metal SIFT disabled; iOS system-only
linkage; framework-local OpenMP on macOS; valid signatures; consolidated
license provenance; and fresh external SwiftPM builds for all three
destinations. Independent ZIP extraction matched all 24 packaged file hashes.

A released-v0.2.1-header client compiled, linked, and ran against the packaged
macOS slice with sparse config/result sizes 136/1088 and an unchanged
post-result guard. Two packaged macOS runs each emitted and validated exactly
4096 rows, reported 325534076 admitted bytes, and produced identical artifact
SHA-256
`f9eaaaa2f6cac8dae923d47bd6fb782ad835f20c8bb5abf0227675f8dc70a9ec`.

## Physical attempt

The committed matrix is
`doc/colmapkit_frame_feature_rc2_physical_predeclaration.md`, SHA-256
`f657d2705d3870a76e4ed50fdd3ef223f9c3aa888a8fed20e8714ecfcdea6fff`.
The prelaunch identity receipt is
`dist/colmapkit-v0.3.0-rc2-670a14bb/audits/physical-prelaunch-identity.txt`,
SHA-256
`e2547254404d13f0b7cbc6a76cdc333010b36dd5d195ae7f37a5a3a19d97349a`.

The single preserved attempt is:

`/Users/brw/Developer/ai-projects/colmap/dist/colmapkit-v0.3.0-rc2-670a14bb/physical/attempt-01-count-contract-full-matrix`

Its file-hash manifest is
`dist/colmapkit-v0.3.0-rc2-670a14bb/audits/physical-attempt-01-file-hashes.txt`,
SHA-256
`64bcd95017e4eb3213d30ef5e64f0681a22fb9f32057a2611daa2220b4dfbc1b`.

The device was the required wired, paired, unlocked M1 iPad: CoreDevice
`302F7720-91AC-5103-A977-592E006C1FF7`, UDID
`00008103-0012086A1179001E`, `iPad13,6`, iPadOS 27.0 build `24A5370h`.
No competing benchmark or harness was present. The signed embedded framework
matched the package after signature normalization:
`e96f65acf33aaaee74eba7f300cd91c2e38ec9aebd8ea6f09b2aa771a8fdfaad`.

Preflight proved:

- exact runtime/source/fixture identity;
- CPU backend, one worker, 1024/4096/two-orientation profile;
- repaired maximum estimate `325534076` bytes under the unchanged
  `335544320`-byte budget;
- 56852480 resident bytes at start;
- 10992610232 available-storage bytes;
- battery 20%, unplugged, Low Power Mode false, thermal nominal.

The harness reached the final resource gate. By control-flow construction,
the preceding guards had already completed repeated extraction/validation,
the exact representative 4096-row assertion, cancellation/cleanup, two
three-frame transactional imports, invalid/conflict/partial cases, strict
Metal rejection, ten extraction lifecycles, and two import lifecycles. The
candidate then failed:

```text
Resource gate failed peak=316653568 growth=257228800 samples=5
```

The gate-time terminal RSS was therefore 314081280 bytes. The separately
encoded failure receipt observed 314310656 bytes moments later. Both exceed
the allowed terminal growth of 134217728 bytes over the 56852480-byte start;
peak RSS remained below the independent 805306368-byte ceiling. Thermal was
still nominal, available storage was 10994637752 bytes, battery remained 20%
and unplugged, and Low Power Mode remained false. Before/after device process
inventories are byte-identical, and the harness terminated rather than
remaining resident.

## Classification and proof limits

This is not the prior admission-profile rejection: the repaired estimator was
accepted under 320 MiB. It is not a package identity or count-bound failure:
the harness reached the terminal resource check only after those guards. It is
an owner-package lifecycle/resource acceptance failure observed at process
RSS after all declared job/context releases.

The retained evidence does **not** distinguish a true allocation leak from
allocator, image-decoder, SIFT, or dependency cache/high-water retention.
Because the failure-only receipt intentionally omits the accumulated success
payload, exact per-operation artifact/import hashes, counts, clocks, and the
five individual resource-sample values are not externally available even
though their guards ran. Those missing receipts prevent a physical acceptance
claim for the count repair independently of the terminal failure.

The smallest unstarted next owner question is a diagnostic-only lifecycle
localization on these same RC2 bytes: record RSS and allocator/cache facts
after each decode, extraction, job release, extractor release, import release,
and lifecycle iteration to identify the first retained working set. That is a
new experiment and was not started here. No second source fix, resource-gate
change, rerun, budget, threshold, fixture, or product-policy adjustment was
made.

## Decision and boundaries

**Reject RC2. Do not create the final local v0.3.0 goal, release, publish, or
pin this package.** The package remains useful only as immutable diagnostic
evidence. RC1 and attempts 1 through 3 remain unchanged.

No Splats, BrushKit, or msplat repository was edited; Splats was not installed
or run. No push, PR, remote CI, tag, release, publication, public SwiftPM
update, consumer pin, product-default change, or manual-quality claim occurred.
