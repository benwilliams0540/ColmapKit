# ColmapKit v0.3.0 publication receipt

Status: published from the exact physically accepted local archive on
2026-08-21. No package rebuild, re-archive, source substitution, consumer edit,
or product-default change occurred.

## Locked local authority

- owner acceptance HEAD before publication:
  `0d7a903033988479ed284c57ab863c30a52bbd23`
- exact package source and runtime:
  `bb91933693e1a1d26e43ccc2358e3dcc2e702caa` / `0.3.0`
- accepted archive:
  `dist/colmapkit-v0.3.0-final-bb919336/ColmapKit.xcframework.zip`
- bytes: `43213354`
- SHA-256 / SwiftPM checksum:
  `ec42eefbcb91d9741d7a7cfd5f4e4d8cc35fd9dbe7991733d3ded2656320caf3`
- acceptance record SHA-256:
  `d1d3d9e87e1328ef679e3b7d1e17ad9c0eec035b157d7e7e48829a543545324d`
- exact-final physical `result.json` SHA-256:
  `b92be40323b9d4156b67c0bd2f51bd21dd06d14c813b306d41914471fcf62f03`
- public header / module map / consolidated notices SHA-256:
  `73fe2af23e3cb2811add36ffb65ea020330f209788a743296a33eb18e5974d25`,
  `d0af903600e0e3dcc4846ace8a88fcf64698daa36b489ce9a41aedfa60a0b9be`,
  and `82b6522d3fba08354eff2d3c5dc77266c4c0d30b406374ab45b208f5f229d0a4`.

Immediately before the first external mutation, the checkout was clean on
`integrate/upstream-main-2026-08`; local and remote release/tag collision
checks found no `colmapkit-v0.3.0` or `v0.3.0`; GitHub authentication was valid;
and every value above was recomputed from the retained files. All three slice
headers, module maps, and notices independently reproduced the locked hashes.

## Branch and annotated tag

The owner branch was published as
`origin/integrate/upstream-main-2026-08`. The annotated tag is:

- name: `colmapkit-v0.3.0`
- tag object: `60ac4b4581ddd3cd9679565d7e10cfed2df734f9`
- peeled commit: `bb91933693e1a1d26e43ccc2358e3dcc2e702caa`
- message: `ColmapKit v0.3.0`

The tag deliberately points at the exact packaged source rather than the later
harness/acceptance-doc HEAD. The public release notes identify both revisions
so the runtime and source provenance are not conflated.

## Public GitHub release and SwiftPM surface

- release: `ColmapKit v0.3.0`
- release ID: `374394374`
- URL:
  `https://github.com/benwilliams0540/ColmapKit/releases/tag/colmapkit-v0.3.0`
- published: `2026-08-21T12:47:01Z`
- state: public, non-draft, non-prerelease, Latest
- asset ID: `523733168`
- public asset URL:
  `https://github.com/benwilliams0540/ColmapKit/releases/download/colmapkit-v0.3.0/ColmapKit.xcframework.zip`
- GitHub asset size: `43213354`
- GitHub asset digest:
  `sha256:ec42eefbcb91d9741d7a7cfd5f4e4d8cc35fd9dbe7991733d3ded2656320caf3`
- release-notes SHA-256:
  `68bf777c76bd52e8abdcb4ffb53715c94e037c6cce0c1fee9af86d194d0ff5c6`

The established ColmapKit package topology does not use a root
`Package.swift`; consumers declare the immutable GitHub Release URL and
checksum as a binary target. The release notes publish the authoritative
copy-ready `.binaryTarget` manifest entry. No second semantic tag scheme or
consumer-local package manifest was introduced.

The release was created as a draft with the accepted local ZIP. Before it was
made public, the uploaded asset reported the locked GitHub digest and an
authenticated download matched the local file using `cmp`, byte count,
SHA-256, SwiftPM checksum, and ZIP integrity. After publication, a second
download using unauthenticated `curl` again matched the accepted local archive
on every check.

## Public SwiftPM consumer verification

A fresh package at
`dist/colmapkit-v0.3.0-final-bb919336/audits/public-swiftpm-consumer/`
references only the public release URL and locked checksum. Its manifest and
source SHA-256 values are:

- `Package.swift`:
  `d497c9ebf93a527170d047011e43bf914ed038ed3f962c94bb548e58a03cd4dc`
- `ColmapKitPublicConsumer.swift`:
  `ec1c9fee9b9246c8048dc9a5fe3b218bb4e22aa0be5c2fcdb7b09bf656c207eb`

Fresh scratch builds downloaded/resolved the public binary target and compiled
the legacy, tracked-pose, RGB-prior, extraction, import, cancellation, and ABI
surface for all three destinations:

- `arm64-apple-macosx15.0`: passed;
- `arm64-apple-ios18.0`: passed; and
- `arm64-apple-ios18.0-simulator`: passed.

## Release-triggered GitHub Actions

GitHub triggered five workflows at tagged source
`bb91933693e1a1d26e43ccc2358e3dcc2e702caa`. The receipt was closed after
continuous monitoring through the first approximately 15 minutes; the three
long-running build matrices had not yet reached terminal status:

- [COLMAP (Mac), run `32483546066`](https://github.com/benwilliams0540/ColmapKit/actions/runs/32483546066):
  in progress in `Configure and build`;
- [COLMAP (Windows), run `32483546143`](https://github.com/benwilliams0540/ColmapKit/actions/runs/32483546143):
  in progress in both CUDA and non-CUDA `Configure and build` jobs;
- [PyCOLMAP, run `32483546053`](https://github.com/benwilliams0540/ColmapKit/actions/runs/32483546053):
  in progress in the Windows, Ubuntu, Ubuntu CUDA, and macOS wheel-build jobs;
- [COLMAP (Ubuntu), run `32483546103`](https://github.com/benwilliams0540/ColmapKit/actions/runs/32483546103):
  failed at the repository-wide format
  gate before build; the formatter reported pre-existing source formatting
  differences in the exact accepted/tagged source and fail-fast cancelled the
  remaining Ubuntu matrix; and
- [COLMAP (Docker), run `32483546062`](https://github.com/benwilliams0540/ColmapKit/actions/runs/32483546062):
  failed before build at Docker Hub login
  because the repository has no Docker Hub username/password configured.

The two known failures do not indicate archive-byte, SwiftPM, ABI, Apple
package, or physical-runtime drift. They are preserved rather than repaired:
formatting the tagged source would create a different source identity and
rebuilding/replacing the accepted archive is outside this publication goal;
Docker Hub credentials are external repository configuration. The nonterminal
matrices are explicitly not counted as passes. No workflow was rerun, cancelled,
or weakened, and each linked run remains the authoritative terminal-status
surface.

## Publication boundary

This publication did not edit Splats, BrushKit, or msplat; install or run an
app; change a product default; pin a consumer; create a v0.3.1 candidate; or
perform unrelated cleanup. The public release makes the owner artifact
available, but downstream adapter/pin/product acceptance remains separately
authorized work.
