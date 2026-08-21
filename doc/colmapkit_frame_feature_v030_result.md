# ColmapKit v0.3.0 final local package result

Status: **retained and recommended for release review**. The immutable RC3
package passed its predeclared physical matrix, and the exact final local
`v0.3.0` package then passed the complete three-slice, compatibility,
external-consumer, and exact-byte physical acceptance gates. No tag, push,
pull request, remote CI, release, publication, public SwiftPM update, or
consumer pin occurred.

## Source and commit topology

- accepted RC3 engine source:
  `14eadc5cad82eb7a53b52d8739b4345054f15d46`
- exact final package source:
  `bb91933693e1a1d26e43ccc2358e3dcc2e702caa`
- device identity parameterization:
  `97defc36df829a3098d3fad9008ef7c78a51d8db`
- immutable final matrix declaration:
  `c386344e38a6cfbe20c6475749872c0a26fb2458`

The package source differs from the physically accepted RC3 engine source only
through owner documentation and the bounded frame-feature device/readiness
harness. No ColmapKit engine, public ABI, extraction/import algorithm, feature
profile, estimator, fallback, or product default changed between them. The
final package was built from the clean `bb919336` tree before the two later
test/documentation commits; its embedded identity is therefore exact.

## Preserved RC3 acceptance

The accepted RC3 identities remain:

- source: `14eadc5cad82eb7a53b52d8739b4345054f15d46`
- runtime: `0.3.0-rc.3+14eadc5c`
- ZIP SHA-256:
  `fe7c88966b0f4f3e67d003a0d0160d40a716590db769a465f732ffbc31d48531`
- frozen matrix SHA-256:
  `758bed5ae71ea8968a6e4e88c74869459826356633fbba86ebe77d1e62842f92`
- retained attempt:
  `dist/colmapkit-v0.3.0-rc3-14eadc5c/physical/attempt-02-authorized-full-matrix-20260821`
- RC3 `result.json` SHA-256:
  `2fbcd0c2d9ed1160805e5ad911d0ca52ceb5cac84e1b9cc2ba5be12ec054b629`

RC3 passed exact package identity, CPU/one-worker/no-fallback execution,
repeat artifacts, cancellation, two deterministic imports, all fail-closed
cases, ten extraction and two import lifecycles, the 768 MiB peak-RSS gate,
the 128 MiB terminal-growth gate, and the power/thermal/evidence gates. RC1,
RC2, their rejections, and every earlier attempt remain unchanged.

## Exact final package

- package directory:
  `/Users/brw/Developer/ai-projects/colmap/dist/colmapkit-v0.3.0-final-bb919336`
- XCFramework:
  `/Users/brw/Developer/ai-projects/colmap/dist/colmapkit-v0.3.0-final-bb919336/ColmapKit.xcframework`
- ZIP:
  `/Users/brw/Developer/ai-projects/colmap/dist/colmapkit-v0.3.0-final-bb919336/ColmapKit.xcframework.zip`
- ZIP bytes: `43213354`
- ZIP SHA-256 / SwiftPM checksum:
  `ec42eefbcb91d9741d7a7cfd5f4e4d8cc35fd9dbe7991733d3ded2656320caf3`
- runtime release identity: `0.3.0`
- framework version/build: `0.3.0` / `4`
- source revision:
  `bb91933693e1a1d26e43ccc2358e3dcc2e702caa`
- clean source-patch SHA-256:
  `e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855`
- macOS binary SHA-256:
  `2cc3e035386adbf3f0ae704af00d5e29caff22fc1be349622b0fbbac92908a36`
- iPhoneOS binary SHA-256:
  `45aa61ab599f0e9b0e04a15ffe6a2faeb15e9e024ebb95d15c315195caa05242`
- Simulator binary SHA-256:
  `01d9b0edebcc1f4a59b12cdfecbaa7a514a1b4d32f43d37ab4a25a2b18396e8b`
- public header SHA-256:
  `73fe2af23e3cb2811add36ffb65ea020330f209788a743296a33eb18e5974d25`
- module map SHA-256:
  `d0af903600e0e3dcc4846ace8a88fcf64698daa36b489ce9a41aedfa60a0b9be`
- consolidated notices SHA-256:
  `82b6522d3fba08354eff2d3c5dc77266c4c0d30b406374ab45b208f5f229d0a4`

Canonical machine-readable package identity is in `artifact-summary.txt`.
The package was built with Xcode 26.5 build 17F42, SDK 26.5, CMake 4.3.2,
Ninja 1.13.2, and vcpkg commit
`a0b1c8d3a477c1cb4813d8e127a56961707ca42b`.

## Static, ABI, package, and consumer acceptance

The candidate has these independently audited arm64 slices:

| Slice | Platform | Minimum OS |
|---|---|---:|
| `macos-arm64` | macOS | 15.0 |
| `ios-arm64` | iPhoneOS | 18.0 |
| `ios-arm64-simulator` | iOS Simulator | 18.0 |

All slice headers, module maps, and 34-symbol export manifests are identical.
iOS linkage is system-only. The macOS slice's sole non-system dependency is
the framework-local `@loader_path/Frameworks/libomp.dylib`. Profiling and SIFT
Metal are disabled in all build caches, and no `sift.metallib` is packaged.
Signatures, property lists, deployment targets, source identity, 126-input
consolidated notices, and archive integrity passed. An independent extraction
verified all 24 archived payload hashes; the ZIP itself independently matched
the recorded SHA-256 and `swift package compute-checksum` value.

Fresh external SwiftPM consumer builds passed for macOS arm64, iPhoneOS arm64,
and iOS Simulator arm64. Their logs are under
`dist/colmapkit-v0.3.0-final-bb919336/audits/swift-package-consumer/`.

A C client compiled from the published `v0.2.1` header, whose SHA-256 is
`4c22ebb3bd876156e014bd2ba8647a673b64df1b3bb7bcb06399f0d028070057`,
linked and ran against the final macOS framework. It reported runtime commit
`bb919336`, sparse config/result sizes `136`/`1088`, invalid-input status `1`,
and an unchanged 64-byte guard after the legacy result. The receipt is
`dist/colmapkit-v0.3.0-final-bb919336/audits/old-v0.2.1-header-client.log`,
SHA-256
`7b0c7eef444b829ab3ad367b1613cc7387cfa190efbdc93c5af2985c0bd504e7`.

The focused owner suite passed 3/3 in 4.87 seconds:

```text
cmake --build build-colmapkit-v2-dev --target \
  colmapkit_v2_test frame_feature_extraction_test frame_feature_import_test -j 4
ctest --test-dir build-colmapkit-v2-dev --output-on-failure \
  -R 'colmapkit/(colmapkit_v2_test|frame_feature_extraction_test|frame_feature_import_test)'
```

The separately parameterized harness compiled successfully both with its
unchanged RC3 defaults and with the final package release/source/slice/ZIP
identity. No device command was issued during either build-only proof.

## Exact-final physical acceptance

The immutable final matrix is
`doc/colmapkit_frame_feature_v030_physical_predeclaration.md`, SHA-256
`75e242b0a255d8335e3c67fe95eb1ff5d24a049b9098c92a7e2af22b025e0948`.
The single retained execution is:

`/Users/brw/Developer/ai-projects/colmap/dist/colmapkit-v0.3.0-final-bb919336/physical/attempt-01-exact-final-v030-20260821`

Important receipt identities:

- `result.json` SHA-256:
  `b92be40323b9d4156b67c0bd2f51bd21dd06d14c813b306d41914471fcf62f03`
- `runtime.log` SHA-256:
  `c311030b38d6aa86b0ff0ca372fa7cb575981da93e2fa35ae42bd2470aee3e44`
- `build-receipt.txt` SHA-256:
  `cf0a1be3f5effb2b4cf421a187a96e4e8540402737ca16c9227d39c4bd69d499`
- complete attempt-hash manifest SHA-256:
  `a641c68f668f998733e4e69d3f3d7e5cf02439ae199218502ccb895592f81e27`

The exact device was CoreDevice
`302F7720-91AC-5103-A977-592E006C1FF7`, UDID
`00008103-0012086A1179001E`, `iPad13,6`, iPadOS 27.0 build `24A5370h`.
It was wired, paired, booted, unlocked, and free of competing benchmark work.
Battery was 100%/full at both boundaries, Low Power Mode was off, thermal was
nominal throughout, and free storage was 11,948,219,174 bytes at start and
11,929,484,070 bytes at end.

The frozen request remained CPU backend `1`, one worker, no fallback,
1024 maximum dimension, 4096 terminal oriented rows, two orientations,
335544320 admitted-budget bytes, and maximum fixture estimate 325534076 bytes.
The fixture manifest SHA-256 was
`cb1ac79e7592c3bb33903e62ee342596597752f48f7b9f64582ca5102c639e1a`.

Every exact-final physical gate passed:

- runtime reported release `0.3.0` and engine source `bb919336`;
- signature-normalized embedded/source iPhoneOS binary identities matched;
- all three frames returned 4096 keypoints and 524288 descriptor bytes;
- actual backend was CPU, effective workers were one, and no fallback occurred;
- repeated representative artifact and receipt bytes were identical;
- profile SHA-256 was
  `19b544f76cd5735178ab85ef14aeb851d89a2fa7665f3dd0dec3c8bba730cda8`;
- representative artifact/payload/metadata SHA-256 values were respectively
  `f893307904e5038a3e1f0d73349afc228dea209e75ac1a8a04314df7e032b939`,
  `b325a9f7da06bf27d051ec6600f5b887c7c5836b4ac7a3e06f5d77addead7829`,
  and `ed875313a5d04a6e6785ded2801bcd4be3929c93980e228d78c0b88f15a07e3f`;
- cancellation returned status `4`, cleaned all authoritative/temp output,
  and terminated in 0.017564833 seconds;
- two imports each committed three items, one camera, three deterministic
  images, 12288 keypoints/descriptors, and zero match/two-view rows;
- both imports shared database SHA-256
  `69a095a687cede048760f10dd73898390f70817e6af207672a183ec9ab392722`,
  sealed-set SHA-256
  `04b80618de5f1d126e3965cb7520327c202c60c056f074254ed3d1ae73f0ef1f`,
  and receipt SHA-256
  `a060a2a8fe6fc280f1cb9e3cc3c2c2f5dc931a8c555b96887cb838e151d1e9e2`;
- corrupt, truncated, stale, preexisting, duplicate, partial, mixed-profile,
  and destination-conflict inputs all failed closed and preserved foreign data;
- the explicit Metal request rejected before work with no CPU fallback;
- ten extraction and two import lifecycles completed;
- peak engine RSS was 313868288 bytes, below 768 MiB;
- resident start/end were 56524800/87359488 bytes, a 30834688-byte terminal
  increase, below 128 MiB; and
- all five-second and boundary resource samples were present.

The source/provenance identities intentionally change profile, artifact,
payload, sealed-set, and import-receipt hashes from RC3. The underlying feature
count, descriptor count, database identity, fixture identity, request, backend,
worker, fallback, transaction, and quantitative acceptance boundaries remain
unchanged. This is expected versioned provenance, not reconstruction drift.

## Recommendation and remaining proof limits

Recommendation: **approve `v0.3.0` for release publication**, using the exact
`bb919336` source and `ec42eefb...` archive above. This is a technical release
recommendation, not release authorization. Cross-device artifact byte
determinism remains unproved; the current evidence proves deterministic repeats
on the exact macOS fixtures and exact M1 iPad. Splats capture-overlap latency,
product workflow quality, and consumer integration remain downstream gates.

After Ben separately authorizes publication, the safe command sequence is:

```text
git status --short --branch
git tag -a colmapkit-v0.3.0 bb91933693e1a1d26e43ccc2358e3dcc2e702caa \
  -m "ColmapKit v0.3.0"
git push origin colmapkit-v0.3.0
gh release create colmapkit-v0.3.0 \
  /Users/brw/Developer/ai-projects/colmap/dist/colmapkit-v0.3.0-final-bb919336/ColmapKit.xcframework.zip \
  --title "ColmapKit v0.3.0" --generate-notes
```

Then verify the published asset SHA-256 is still `ec42eefb...`, update the
public SwiftPM URL/checksum through a separately reviewed consumer change, and
only afterward authorize an explicit Splats pin. None of those commands or
consumer actions were run in this goal.
