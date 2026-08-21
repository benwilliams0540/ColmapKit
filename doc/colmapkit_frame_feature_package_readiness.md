# ColmapKit Frame Feature Apple Package Readiness

Status: local immutable candidate audited; publication and consumer pin are not
authorized.

## Recommendation

Use `v0.3.0` for the first release containing `FrameFeatureExtractionV1` and
`FrameFeatureImportV1`. These are additive public operation families rather
than a patch-level correction to `v0.2.1`. The audited artifact is deliberately
identified as `0.3.0-candidate.1+695ace6f`; it is not a release artifact and
must not be consumed by Workflow or Splats until Ben separately authorizes the
release and the remaining gates below are closed.

The candidate source is the clean commit
`695ace6ff968f869eb78b93e441858819884a1c2`. Later owner commits only add a
CPU-mode package runtime-audit option and this technical record; they do not
change the packaged library source.

## Candidate identity

- XCFramework:
  `/Users/brw/Developer/ai-projects/colmap/dist/colmapkit-v0.3.0-o3-candidate-695ace6f/ColmapKit.xcframework`
- ZIP:
  `/Users/brw/Developer/ai-projects/colmap/dist/colmapkit-v0.3.0-o3-candidate-695ace6f/ColmapKit.xcframework.zip`
- ZIP bytes: `43212560`
- ZIP SHA-256 and SwiftPM checksum:
  `59e7d1a660d1fe3c044e9dd58adacfb1eaaa37393b9316cabf33587de5718942`
- Runtime release identity: `0.3.0-candidate.1+695ace6f`
- Framework short/build versions: `0.3.0` / `1`
- Source revision: `695ace6ff968f869eb78b93e441858819884a1c2`
- Source patch SHA-256 (clean tree):
  `e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855`
- Public-header SHA-256:
  `41fd5cfea9c168fd33ab7443ffbbfb9d1007e550cd04ea4c7d2b8908778aab1f`
- Module-map SHA-256:
  `d0af903600e0e3dcc4846ace8a88fcf64698daa36b489ce9a41aedfa60a0b9be`
- Consolidated third-party notices SHA-256:
  `82b6522d3fba08354eff2d3c5dc77266c4c0d30b406374ab45b208f5f229d0a4`

The canonical machine-readable summary is
`dist/colmapkit-v0.3.0-o3-candidate-695ace6f/artifact-summary.txt`; complete
slice, export, signature, interface, checksum, and license evidence is under
its sibling `audits/` directory.

## Package and toolchain audit

The XCFramework contains these arm64 slices:

| Slice | Platform | Minimum OS | SDK |
|---|---|---:|---:|
| `macos-arm64` | macOS | 15.0 | 26.5 |
| `ios-arm64` | iPhoneOS | 18.0 | 26.5 |
| `ios-arm64-simulator` | iOS Simulator | 18.0 | 26.5 |

It was produced with Xcode 26.5 build 17F42, CMake 4.3.2, Ninja 1.13,
and vcpkg commit `a0b1c8d3a477c1cb4813d8e127a56961707ca42b`.
All slice headers, module maps, and 34 ColmapKit export names are identical.
The framework identifiers and signatures are valid and bind nonempty
`Info.plist` metadata. The archive was extracted independently and its file
hashes and signatures matched the directory candidate.

iOS slices link only Apple system frameworks and libraries. The macOS slice's
only non-system dependency is its framework-local
`@loader_path/Frameworks/libomp.dylib`. No Homebrew path, host SDK path, or
macOS-26 deployment dependency appears in the deliverable.

The package contains a consolidated notice generated from 126 dependency
license inputs. The notice bytes are identical in all slices; the versioned
macOS framework stores it in `Resources`, while flat iOS frameworks store it
at the framework root so code signing continues to bind the root
`Info.plist`.

## Public profile and compatibility

The exact API and artifact contracts remain defined by
`doc/colmapkit_frame_feature_extraction.md` and
`doc/colmapkit_frame_feature_import.md`.

The supported O3 profile is:

- one immutable canonical-upright encoded frame per extraction;
- CPU-only, one-worker, canonical non-covariant SIFT;
- versioned `.ckfeatures` artifacts with input, configuration, source, and
  payload identities;
- deterministic ordered seal import into a new destination database;
- transactional all-or-nothing camera, image, keypoint, and descriptor rows;
- no overwrite, no live extraction at seal, no CPU/Metal cache mixing, and no
  fallback.

The released sparse and tracked-pose symbols and structure prefixes remain
unchanged. The candidate adds the extraction/import operation families and
requires all 11 new exports during packaging. A C client compiled with the
released `v0.2.1` header (SHA-256
`4c22ebb3bd876156e014bd2ba8647a673b64df1b3bb7bcb06399f0d028070057`)
compiled, linked, and ran against the new macOS framework. It observed the
released configuration/result sizes `136`/`1088` and the expected candidate
engine identity.

Metal SIFT is not compiled in this profile. A strict Metal SIFT request fails
explicitly with status `3` and an evidence message; it does not execute CPU
work or claim Metal execution. The CPU package audit does not request Metal.

## Validation evidence

Focused owner tests passed from a fresh invocation:

```text
cmake --build build-colmapkit-v2-dev --target \
  colmapkit_v2_test frame_feature_extraction_test frame_feature_import_test -j 4
ctest --test-dir build-colmapkit-v2-dev --output-on-failure \
  -R 'colmapkit/(colmapkit_v2_test|frame_feature_extraction_test|frame_feature_import_test)'
```

All three tests passed. They cover bounded ABI validation/writes, extraction
success and repeat-byte identity, corrupt/mismatched/nonfinite input,
cancellation cleanup and zero-output failure, plus import success,
repeatability, rollback, cancellation, duplicate/mixed/stale inputs,
preexisting destinations, races, no-overwrite behavior, and closed base
database handles.

A fresh external Swift package consumer imported the binary target and
referenced the legacy, tracked-pose, RGB-prior, extraction, and import APIs.
SwiftPM builds passed for macOS arm64, iPhoneOS arm64, and iOS Simulator arm64.
Logs are in `audits/swift-package-consumer/`.

The untouched candidate was installed and run only on an iOS 26.5 Simulator.
The CPU runtime receipt is
`dist/colmapkit-v0.3.0-o3-candidate-695ace6f/simulator-runtime-ios265-cpu/result.json`.
It proves legacy reconstruction and post-processing, cancellation, tracked-pose
V2/no-fallback, and RGB-prior/SH0 routes against the packaged binary. The
legacy fixture registered 8/8 images with 606 points, 2449 observations, and
0.3556101571 px mean reprojection error. The tracked route registered 8/8 with
645 points and no fallback. The RGB prior emitted 3825 validated Gaussians at
SH degree 0 and preserved the input pose. Thermal state was nominal at both
receipt boundaries. This is Simulator evidence, not physical-device proof.

## Packaging corrections made during the audit

The audit found and fixed only owner-side packaging/test defects:

1. Required all extraction/import exports and embedded explicit candidate
   release identity.
2. Added consolidated dependency notices and license-input provenance.
3. Replaced an ephemeral Xcode-scheme consumer assumption with deterministic
   SwiftPM destination builds.
4. Populated valid framework name/version/build metadata.
5. Derived the full source revision from Git and rejected caller/source
   identity mismatches or dirty source trees.
6. Preserved flat iOS framework layout so signatures bind the correct
   `Info.plist`.
7. Added a test-harness-only CPU audit mode; strict Metal remains the default
   harness behavior.

No extraction, import, reconstruction, or product-policy algorithm changed.

## Remaining release and O3-C2/O3-C3 gates

The candidate is not presently publishable or consumable. The remaining gates
are:

1. Ben authorizes a `v0.3.0` release and immutable consumer candidate.
2. The candidate suffix is replaced with the authorized final release
   identity and the canonical package is rebuilt from that clean commit.
3. The final archive receives the intended release signing/publication
   treatment and the same slice, ABI, checksum, dependency, license, external
   consumer, and runtime audits are rerun.
4. Physical-device extraction/import runtime, cancellation, transactional
   cleanup, and resource limits are validated. Same-machine repeat-byte
   identity is proven; cross-device byte determinism remains explicitly
   unproved.
5. An authorized tag, push, release asset/public URL, Swift package checksum
   update, and explicit Splats pin are performed in that order.
6. Workflow/Splats integrate O3-C2/C3 only after the immutable package exists;
   package-free O3-C1 remains independent.

No physical-device run, push, pull request, remote CI, tag, release,
publication, consumer edit, consumer repin, or default change occurred in this
audit.
