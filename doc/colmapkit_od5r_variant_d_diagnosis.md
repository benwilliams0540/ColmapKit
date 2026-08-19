# ColmapKit OD5R Variant-D Diagnosis

Date: 2026-08-18

## Verdict

ColmapKit does not own the preserved OD5R failure. Its feature-size-480
tracked reconstruction and RGB surface-aware prior are finite and contract-valid
through the exact `init.ply` bytes consumed by BrushKit. The first recorded
non-finite values occur later, inside BrushKit after its iteration-201 split:
11 terminal transform rows are non-finite at iteration 300. BrushKit prunes
those rows for export, but its completion record retains the pre-prune primitive
count. Splats therefore rejects internally inconsistent terminal accounting.

No ColmapKit clamp, ABI change, engine change, or candidate artifact is
warranted from this evidence.

## Source and evidence identity

- Diagnosis branch: `integrate/upstream-main-2026-08`
- Diagnosis starting commit: `540dc3aa8fce9e080faa3208191b78399d871732`
- OD5R ColmapKit source commit:
  `32d15c99a0778cc7f1ee94f74cf07338e4b5bf68`
- The current `colmapkit_v2.cc` and canonical V2 artifact validator are
  byte-unchanged from that OD5R source commit.
- OD5R evidence root:
  `/Users/brw/Developer/apps/gsplat/build/codex-artifacts/speedy-splats-overnight/phase3-v031-d-revisit-feature-preflight/retrieved/OD5R-v031-feature480-failed`
- OD3R comparison root:
  `/Users/brw/Developer/apps/gsplat/build/codex-artifacts/speedy-splats-overnight/phase3-v031-d-revisit-feature-preflight/retrieved/OD3R-v031-revisit4`

OD5R boundary checksums:

| Boundary | SHA-256 |
|---|---|
| Refined poses | `ffdb9d245434970ed2cca49ddbc4fb5e2e94773238481f87cad5bef485f03793` |
| `cameras.bin` | `03b24fe7d455882be63274cbfe40928d0d028c309b6a55fc87efa40ae617c56a` |
| `images.bin` | `fb1d1dbaf7294cd1a5768bd6be55f9e1ea491b910b6b9aaf658c86fb24943d95` |
| `points3D.bin` | `ad7adebdd382d92c085116838ed07448104879a4208ead0a547fd317c88ea2b4` |
| ColmapKit `init.ply` | `012cc15b14ae5eebd0dac649a75133e8fccff2225e99fa22a64035b4ea7d3b5e` |
| Tracked evidence | `e67b4a33ca7677cd2dc2e46ea2b1df37d82447f9d45760c89ca52de4c6417ee4` |
| Prior evidence | `ebfa6335e083b91753ed6cfece95eb81f06ac2592375166ef8cbe88fb941babc` |
| Brush terminal PLY | `5c9cce1923abc4168c2b13a746f72cf28230c3d7e71776f303d9c33344ab4972` |

The ColmapKit initializer and BrushKit's copied initializer are byte-identical:

```text
workspace/thread/attempts/benchmark/colmapkit-v2/init.ply
workspace/thread/attempts/benchmark/trainer-leaves/pinhole-fullres-component-0/initializers/variant-d.ply
```

Both have SHA-256 `012cc15b...7d3b5e`, and a byte-for-byte comparison exits
successfully. This is the last-good cross-engine file boundary.

## Boundary trace

The repository's canonical parser and validator passed both runs:

```sh
python3 scripts/python/validate_colmapkit_v2_artifacts.py \
  '<OD5R-root>/workspace/thread/attempts/benchmark/colmapkit-v2'
python3 scripts/python/validate_colmapkit_v2_artifacts.py \
  '<OD3R-root>/workspace/thread/attempts/benchmark/colmapkit-v2'
```

OD5R reports status `ok`, 40 registered images, 1,752 sparse points, 11,169
Gaussians, densification ratio 6.375, pose SHA `ffdb9d...3793`, and PLY SHA
`012cc15b...7d3b5e`. OD3R reports status `ok`, 40 registered images, 1,058
sparse points, 6,484 Gaussians, densification ratio 6.128544, pose SHA
`9bee9b76d54008cb8ffe1db23fa2b1561472ed95525966a2dae82504f94950d0`,
and PLY SHA
`4a7d7fb6d9e552aad47b72d4a83e58f98c31d6b40b6e8a0ba620b67d657f67a7`.

### OD5R tracked reconstruction

- Feature maximum image dimension: 480.
- 40 registered images, 1,752 sparse points, 6,151 observations, 114 matched
  image pairs.
- Initial/final mean reprojection error: 14.052680 / 2.707991 pixels.
- Maximum camera correction: 0.124072 metres and 1.809144 degrees.
- Metric scale drift: zero.
- All 640 refined-pose matrix values are finite.
- Rotation determinants span 0.9999999999999928 to 1.0000000000000067;
  maximum orthogonality error is 6.99e-15.
- Camera parameters, sparse coordinates, colours, errors, and track values are
  finite. Sparse reprojection error has median 2.095, p95 5.681, p99 9.259,
  and maximum 264.503 pixels.
- Sparse radius from the origin has median 3.427 m and maximum 187.535 m.
- The route records RGB-only input, no depth, no plane sweep or dense MVS, and
  no fallback.

### OD5R RGB surface-aware prior

- 11,169 rows: 1,703 sparse provenance plus 9,466 RGB-correspondence
  provenance. All ABI and evidence counters agree.
- All 14 values in every row are finite.
- Position radius: median 3.427 m, p95 18.208 m, p99 53.897 m, maximum
  2,113.703 m.
- Tangent scales: 0.0008 to 0.2 m; normal scales: 0.0002 to 0.05 m.
- Surface-tangent anisotropy is 4 within float precision.
- Quaternion norms span 0.999999958 to 1.000000042.
- Opacity decodes to 0.1 within float precision.
- SH0/DC decodes to RGB from approximately -1.5e-8 to 1.000000015, within
  the validator's float-rounding tolerance.
- SH degree is 0; no higher-order coefficients are invented.

OD5R has a notable but finite far-point tail. Relative to its 6.745 m camera
trajectory diameter, 47 prior rows are more than 100 m from their nearest
camera, three more than 250 m, two more than 500 m, and one more than 1,000 m.
The furthest row is RGB-correspondence provenance at approximately
`[-2051.063, 9.353, -510.681]`, 2,111.952 m from its nearest camera, with the
bounded `[0.2, 0.2, 0.05]` m scale and opacity 0.1.

This is not evidence of the preserved failure by itself. OD3R also has a
finite far-point tail: 24 rows beyond 100 m, six beyond 250 m, and a maximum
nearest-camera distance of 450.075 m. OD3R completed training without any
non-finite terminal row. The current prior contract bounds density and
Gaussian scale, not scene radius, so adding an unrequested scene-distance
clamp here would redefine valid output and hide rather than establish cause.

## First invalid boundary

BrushKit evidence for OD5R records:

1. Initialization: 11,169 input rows, 11,169 accepted primitives, zero
   rejected rows, and zero non-finite prunes.
2. Iteration 201: 2,041 primitives added by a split and one primitive pruned,
   for a net increase of 2,040 and a primitive count of 13,209.
3. Iteration 300 terminal validation: source count 13,209, exported count
   13,198, `transforms_non_finite_rows=11`, with zero non-finite SH or opacity
   rows.
4. Completion: the record still reports final primitive count 13,209 even
   though the terminal export contains 13,198 rows after pruning the 11 invalid
   transforms.

Splats reports: `BrushKit V2 evidence validation failed: the terminal native
primitive accounting is inconsistent`.

OD3R provides the control: 6,484 accepted initial rows, a 1,278-row split at
iteration 201, no non-finite prune, and consistent source/export/completion
counts of 7,762 at iteration 300.

The first invalid values are therefore BrushKit-owned, post-initialization,
post-split transform rows. A separate downstream repair should both prevent or
identify those transform instabilities and make completion accounting use the
validated exported count after terminal pruning.

## ABI and release impact

- ColmapKit source changes: none.
- Released legacy ABI changes: none.
- Tracked-pose/RGB-prior ABI changes: none.
- Depth or fallback behavior changes: none.
- Candidate XCFramework: not warranted.
- Packaging, publication, pinning, push, PR, CI, tag, and release actions:
  none.

## Read-only Standard COLMAP worker follow-up

The preserved M1 iPad worker-screen report does not show W6 or W8 engine
failure. All four candidates produced the required sparse model files and
registered all 115 images:

| Workers | Wall time (s) | Peak RSS (bytes) | Points | Observations | Mean reprojection error |
|---:|---:|---:|---:|---:|---:|
| 1 | 365.243 | 844,382,208 | 45,566 | 176,057 | 1.20455 |
| 4 | 164.249 | 1,492,582,400 | 45,594 | 175,997 | 1.20336 |
| 6 | 169.203 | 1,946,468,352 | 45,960 | 177,279 | 1.20762 |
| 8 | 194.525 | 2,286,075,904 | 46,184 | 178,203 | 1.21104 |

The records remain CPU-only, legacy config/result sizes 136/1088, thermal
state nominal, and required sparse files present. W6 and W8 are negative
scaling results rather than reconstruction failures: they are slower than W4
while consuming approximately 454 MB and 793 MB more peak memory,
respectively. Files named `checkpoint-failed` are partial/empty harness report
snapshots; they do not contain a failed W6/W8 reconstruction. No new device
campaign was run.

## Copy-ready downstream handoff

> ColmapKit ownership verdict for OD5R: no engine defect was found and no
> ColmapKit change or artifact is proposed. The exact ColmapKit `init.ply`
> (`012cc15b14ae5eebd0dac649a75133e8fccff2225e99fa22a64035b4ea7d3b5e`,
> 11,169 rows) passes the canonical V2 validator with every pose, sparse-model,
> mean, log-scale, quaternion, opacity, and SH0/DC value finite. The initializer
> copied into BrushKit is byte-identical, and BrushKit accepts all 11,169 rows
> with zero rejects/non-finite prunes. The first invalid boundary is BrushKit
> iteration-300 terminal validation after the iteration-201 split: 11 of
> 13,209 transform rows are non-finite; SH and opacity rows remain finite.
> BrushKit exports 13,198 rows after pruning them but completion evidence still
> reports 13,209, which is why Splats rejects terminal primitive accounting.
> Please fix/diagnose post-split transform stability in BrushKit and make final
> primitive accounting reflect the validated post-prune export. OD5R does have
> a more extreme but finite far-point tail than OD3R; because OD3R also contains
> far points and completes, and the ColmapKit contract bounds Gaussian scale
> rather than scene radius, do not treat a cosmetic Colmap clamp as the repair.
