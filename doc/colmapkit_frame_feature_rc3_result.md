# ColmapKit RC3 lifecycle/RSS result

## Decision

**RC3 is statically valid but physically unaccepted. Do not release, publish,
or pin it.** The bounded Apple SIFT workspace repair passed focused owner tests,
exact-feature equivalence, the unchanged 128 MiB terminal-RSS gate on macOS,
and the complete three-slice package audit. The sole authorized physical
attempt then stopped at the predeclared device preflight because the exact M1
iPad was unplugged at 15% battery, below the unchanged 20% minimum. No feature
work or import work ran on the device.

This is an environmental preflight rejection, not evidence of an RC3 engine,
ABI, package, cancellation, transaction, or lifecycle failure. The exact RC3
bytes remain a technically eligible subject for a separately authorized future
physical attempt after the device satisfies the gate. This goal does not rerun
them. The automatic final-local-v0.3.0 goal is not started because RC3 did not
pass physical acceptance.

## Ownership diagnosis and repair

Exact RC2 diagnostics showed that after two sequential feature jobs had
released their jobs, extractors, results, and encoded input, live malloc grew
only 217392 bytes while process RSS remained about 254 MiB above its starting
boundary. A second complete extraction added only 131072 terminal RSS bytes.
The retained pages corresponded to the four fixed VLFeat first-octave image
workspace buffers: temp, octave, DoG, and gradients. For the 1024 by 768,
first-octave -1 profile those buffers total 276824064 bytes.

Commit `14eadc5cad82eb7a53b52d8739b4345054f15d46` changes only those four Apple
workspace allocations to private anonymous mappings and unmaps them on filter
destruction. Mapping failure fails extractor creation. Non-Apple builds and
all other allocations retain the VLFeat allocator. There is no pressure-relief
call, delay, garbage-collection workaround, feature truncation, algorithm,
configuration, ABI, schema, fallback, or product-policy change.

The diagnostic implementation/predeclaration commit is
`a84eeb9b`, and the repair commit is `14eadc5c`. The authoritative exact-source
diagnostic is:

`/Users/brw/Developer/ai-projects/colmap/build/colmapkit-frame-feature-rss-diagnostic-14eadc5c-exact`

Its hashes are copied to
`dist/colmapkit-v0.3.0-rc3-14eadc5c/audits/mac-rss-diagnostic-hashes.txt`
(SHA-256
`992650355032b2b8325c89ea82817204a24db128d30a77160eab709c0678e165`).
A preliminary macOS diagnostic with a mistyped full source-revision string is
preserved separately at
`build/colmapkit-frame-feature-rss-diagnostic-14eadc5c-rejected-source-identity`
and is not used as RC3 evidence.

Exact-source diagnostic facts:

- two CPU, one-worker, no-fallback runs completed;
- both returned 4096 features, 524288 descriptor bytes, and 325534076 admitted
  bytes;
- both artifact hashes are
  `50f1282ffa84df854eab2f6099e543442dc3f7d4171dc93b7cfa11114572d331`;
- their 4096 six-float keypoint rows and 524288 descriptor bytes are
  byte-identical to the immutable RC2 output;
- process-start RSS 38223872, run-one extractor-release RSS 65781760,
  run-two extractor-release/final RSS 65880064;
- terminal growth 27656192 bytes, below 134217728;
- second-run terminal increment 98304 bytes, below 16777216;
- final live malloc 25877120 bytes, below 33554432;
- in-operation peak remained 292405248 bytes and fits the unchanged 320 MiB
  admission policy.

Focused validation passed:

```text
feature/sift_test
colmapkit/colmapkit_v2_test
colmapkit/frame_feature_extraction_test
colmapkit/frame_feature_import_test
```

All four passed, with no public ABI or header edit.

## Immutable RC3 package

- source: `14eadc5cad82eb7a53b52d8739b4345054f15d46`
- runtime: `0.3.0-rc.3+14eadc5c`
- path:
  `/Users/brw/Developer/ai-projects/colmap/dist/colmapkit-v0.3.0-rc3-14eadc5c`
- ZIP bytes: `43213395`
- ZIP SHA-256 / SwiftPM checksum:
  `fe7c88966b0f4f3e67d003a0d0160d40a716590db769a465f732ffbc31d48531`
- iPhoneOS binary:
  `1c34a7cc27a35c0004c6846dcfdc7c632c1fae60fe77a5ec6b95a72b089695d7`
- macOS binary:
  `1cec8fa7a518ee76a9f52f4ef3640dc803b89adf7f2b4d81495bc3e7ca3bbc34`
- Simulator binary:
  `917b80d44f8b3c052cbac66c5f6c2aa50c5d29422e1c53262aa257c540eef4c0`
- public header:
  `73fe2af23e3cb2811add36ffb65ea020330f209788a743296a33eb18e5974d25`
- module map:
  `d0af903600e0e3dcc4846ace8a88fcf64698daa36b489ce9a41aedfa60a0b9be`
- notices:
  `82b6522d3fba08354eff2d3c5dc77266c4c0d30b406374ab45b208f5f229d0a4`

The package has macOS arm64 minimum 15.0, iPhoneOS arm64 minimum 18.0, and iOS
Simulator arm64 minimum 18.0 slices. Headers/module maps and expected exports
are identical. iOS uses system-only linkage; macOS contains its audited local
OpenMP dependency. SIFT Metal and profiling are disabled. Codesign, plist,
deployment, linkage, license/notice, archive, current/released-header ABI, and
fresh external SwiftPM consumer checks passed for every buildable destination.
The first SwiftPM consumer attempt was blocked only by the restricted process
being unable to write `~/.cache`; the exact immutable audit was rerun outside
that restriction and passed. No source/package identity changed.

## Physical attempt and stop boundary

The matrix was committed at `883a26d0` before the RC3 device query. Matrix:

`/Users/brw/Developer/ai-projects/colmap/doc/colmapkit_frame_feature_rc3_physical_predeclaration.md`

SHA-256:
`758bed5ae71ea8968a6e4e88c74869459826356633fbba86ebe77d1e62842f92`.

Attempt:

`/Users/brw/Developer/ai-projects/colmap/dist/colmapkit-v0.3.0-rc3-14eadc5c/physical/attempt-01-lifecycle-full-matrix`

Attempt hash manifest:

`dist/colmapkit-v0.3.0-rc3-14eadc5c/audits/physical-attempt-01-file-hashes.txt`

Manifest SHA-256:
`2b22c2911664cb58f76cbf53c3ffa85f4e36772693458854388fa879be799218`.

The build receipt proved the exact ZIP, matrix, fixture manifest, runtime,
iPhoneOS framework, and signature-normalized embedded framework. The device
was the expected wired, paired, booted, unlocked M1 iPad13,6, UDID
`00008103-0012086A1179001E`, on iPadOS 27.0 build `24A5370h`. No competing
host benchmark/build or device harness was active. Device preflight reported:

- available storage 10970606520 bytes;
- thermal nominal;
- Low Power Mode false;
- process RSS 53706752 bytes;
- battery 15%, unplugged.

The harness failed before extraction with: `Refusing to start below 20%
battery or with unknown battery state.` No profile-bearing feature result,
artifact, cancellation run, import, invalid-input matrix, repeat lifecycle, or
post-run resource series exists. None may be inferred from macOS/static proof.

## Publication and next gate

No Splats, BrushKit, or msplat edit; Splats install/run; push; PR; remote CI;
tag; release; publication; public SwiftPM update; consumer
pin; default change; or manual-quality claim occurred. RC1 and RC2 packages,
attempts, and records remain unchanged.

The next action is user-owned coordination, not started here: charge/connect
the exact iPad until it is at least 20%, then separately authorize whether the
same immutable RC3 bytes may receive a new uniquely named full physical
attempt. Only an all-gates RC3 pass may open the final-local-v0.3.0 goal.
