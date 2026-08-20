# ColmapKit tracked-pose pair-graph experiment

Date: 2026-08-19

## Decision

Reject the only tested pair-cap candidate (`max_image_pairs=436`). It repairs
the explicit pair graph from 51 components and 50 isolated images to one
component with no isolated images, but constrained bundle adjustment produces
three finite yet pathological point errors above `1e100` and a final mean
reprojection error of `1.8899142887266853e150` in both runs. Do not package,
pin, train from, or run this candidate on a physical device.

Keep the completed Standard-COLMAP Metal experiment closed. This experiment
used the tracked-pose CPU route only, consumed no depth, used no fallback, and
did not change BrushKit or Splats.

## Source and immutable input

- Branch: `integrate/upstream-main-2026-08`
- Experiment implementation: `8c34f6251ee031de17fdf8222806e8d72ed582c8`
- Starting commit: `102f9119d11ce31883cfefa553f7d8a4265ae4ed`
- Retained capture manifest:
  `/Users/brw/Developer/apps/gsplat/build/codex-artifacts/device-fast-preview-build235-user-success/ipad-scene-a17a539c-aef6-4025-b45f-676f29f31ecd/attempts/starter-20260819-145417/on-device-training/guided-3df5bf68-3aaa-4c0f-8dad-e523a87b7404/on-device-capture/capture-manifest.json`
- Capture-manifest SHA-256:
  `0ab979b4214b86a1aeb04f680f2e06040707123694c7bacd6309a073398983b6`
- Image-set SHA-256, using the consumer's name/size/content manifest:
  `1171459bec06367752f4ebcd16e63c1afdc8ec5d055b35fde2ea8e633c24567c`
- Input facts revalidated: 104 JPEGs, 353,263,922 total bytes, 104 normal
  tracking poses, 104 positive finite intrinsics, one coordinate frame
  `df66a1e2-b676-4642-b460-1a016c90aba6`, and exact manifest file hashes.
- Frozen policy: feature maximum dimension 360, 4,096 features/image,
  temporal neighbors 3, revisit neighbors 2, two triangulation passes, 40 BA
  iterations, four requested threads, 0.1--2.0 m revisit translation, 45
  degrees revisit rotation, 0.1 degree minimum triangulation angle, 4 px
  reprojection threshold, ARKit pose weights 1.0, no depth, and no fallback.

The physical receipt is an older source identity,
`32d15c99a0778cc7f1ee94f74cf07338e4b5bf68`, at 5,641 points, 19,962
observations, and 2.202494930748282 px. Current integrated source produces
5,619 points, 19,876 observations, and 2.208842217479839 px on the same input
and policy. That is the first reproduction identity boundary; the current
experiment does not relabel its integrated-source control as an exact replay of
the older physical artifact.

## Default-off telemetry

`COLMAPKIT_TRACKED_POSE_FLAG_V2_PAIR_GRAPH_TELEMETRY` is an opt-in bit in the
existing `flags` field. Neither tracked-pose struct grew: config/result remain
160/1,208 bytes. With the bit absent, the existing evidence schema and output
path remain unchanged. With it present, `tracked-evidence.json` adds:

- temporal/revisit attempts and rejection reasons;
- generated, selected, raw-match, and geometrically verified pair counts;
- first cap-truncated pair and selection-order identity;
- generated/selected/verified graph components, degrees, and isolated images;
- per-image keypoint, selected/verified degree, and final observation counts;
- per-pair raw matches, verified inliers, shared final points, and observations;
- final track-length histogram.

The 200-pair telemetry-on runs, telemetry-off run, and repeated controls have
identical pair-list, refined-pose, camera, frame, image, point, and rig hashes.
The opt-in telemetry therefore did not change reconstruction results. The
synthetic tracked-pose plus Variant-D fixture also passed with the flag enabled.

## First coverage loss and candidate declaration

The existing selector first creates a `std::set` of unique temporal/revisit
pairs and then truncates that lexicographic input-index order. On this capture:

- 306 temporal attempts and 10,100 revisit attempts generate 455 unique pairs.
- Revisit rejections: 98 below minimum translation, 8,454 above maximum
  rotation, 1,340 beyond the per-image neighbor limit, and 59 duplicate
  undirected candidates. None exceed maximum translation.
- The full generated graph is connected, has no isolated images, minimum
  degree 5, maximum degree 14, and mean degree 8.75.
- The 200-pair prefix has 51 components, 50 isolated images, a 54-image largest
  component, minimum degree 0, maximum degree 14, and mean degree 3.84615.
- Every selected baseline pair has raw matches and passes geometric
  verification: 200 accepted pairs and 38,417 verified inliers.
- The first omitted edge is frame 42--49; the last generated edge is frame
  103--104.

The smallest cap that connects the unchanged lexicographic candidate set is
436. That value was predeclared as the single experiment. No feature,
neighbor-policy, threshold, triangulation, BA, worker, Metal, depth, fallback,
or training dimension changed.

## Balanced macOS result

Both lanes ran in alternating baseline/candidate order on the same host. Values
below are two-run medians; reconstruction outputs were identical within each
lane at the metric level.

| Metric | 200-pair control | 436-pair candidate |
|---|---:|---:|
| Selected / verified pairs | 200 / 200 | 436 / 435 |
| Verified graph components / isolated | 51 / 50 | 1 / 0 |
| Verified inliers | 38,417 | 80,731 |
| Registered images | 104 | 104 |
| Sparse points | 5,619 | 11,824 |
| Observations | 19,876 | 40,849 |
| Observations per point | 3.5373 | 3.4548 |
| Final mean reprojection error | 2.208842 px | `1.8899142887e150` px |
| Feature extraction | 8.7138 s | 8.7318 s |
| Matching | 0.2678 s | 0.5656 s |
| Triangulation | 0.0623 s | 0.1248 s |
| Bundle adjustment | 0.5280 s | 0.3122 s |
| Export | 0.7867 s | 0.8846 s |
| Reconstruction call wall | 10.4018 s | 10.6748 s |
| Peak resident bytes | 299,917,312 | 358,891,520 |

The candidate adds 110.43% points and 105.52% observations but decreases
observations per point by 2.33%, takes 2.62% more wall time, and uses 19.66%
more peak resident memory. Its lower reported BA duration is not a speed win;
the solved model is pathological.

All candidate values are finite, so the first bad distribution is after BA,
not feature selection or matching. Each run contains exactly three point errors
above `1e100`; the worst is point 10,532, track images 88/87/89, with error
`1.3407807929942596e154`. Median and p99 point errors remain approximately
1.8225 and 9.8147, showing a small catastrophic tail rather than a global NaN.
The two candidate runs also have different pose/frame/image/point binary hashes
despite identical headline metrics. This is an additional rejection signal.

## Commands and evidence

Configure/build with exact source identity:

```bash
cmake -S . -B build-colmapkit-v2-dev \
  -DCOLMAPKIT_SOURCE_REVISION=8c34f6251ee031de17fdf8222806e8d72ed582c8
cmake --build build-colmapkit-v2-dev \
  --target colmapkit_tracked_capture_runner colmapkit_v2_fixture_runner \
  colmapkit_v2_test -j 4
build-colmapkit-v2-dev/src/colmap/colmapkit/colmapkit_v2_test
```

Run a lane (replace the output directory and pair cap for each alternating
run):

```bash
build-colmapkit-v2-dev/src/colmap/colmapkit/colmapkit_tracked_capture_runner \
  /Users/brw/Developer/apps/gsplat/build/codex-artifacts/device-fast-preview-build235-user-success/ipad-scene-a17a539c-aef6-4025-b45f-676f29f31ecd/attempts/starter-20260819-145417/on-device-training/guided-3df5bf68-3aaa-4c0f-8dad-e523a87b7404/on-device-capture/capture-manifest.json \
  build/codex-artifacts/tracked-pose-pair-cap/final-baseline-200-run1 200
```

Pass a final `0` argument to disable telemetry and prove parity. Each run
contains `runner-result.json`, `tracked-evidence.json`, the explicit pair list,
database, refined poses, and sparse model. Evidence root:

`/Users/brw/Developer/ai-projects/colmap/build/codex-artifacts/tracked-pose-pair-cap`

Key pair-list SHA-256 values:

- 200 pairs: `ffd922502cc54576c4cbfc88bf819bb7c299f750b704892add687317e93410a8`
- 436 pairs: `e972ec48beae9eea2b0d5debbe0e234ed611038bcc310a97b833f942aeb80ce5`

The current-source controls are byte-stable across two telemetry-on runs and
the telemetry-off run. Their shared sparse `points3D.bin` SHA-256 is
`f181463b2f108c3f8baa7f3f1a5e451a233939e9f523c16846b705be43db8fb1`;
their shared refined-pose SHA-256 is
`70f0d3d4374500477264b4dfc01bef74b6b9b6c7db992f981b2aaf5c6896a093`.

Executable SHA-256 values:

- retained-capture runner:
  `a7dc46032257d6705dcc31bc5e78aaa20d275ee3c5f2476ac7f7c2b7e4023188`
- focused V2 test:
  `1849906ea2e6607d9f01dc5758cca07d0ab1748783dc7e5aac470ec62e401490`

Per-run result/evidence SHA-256 values:

| Run | `runner-result.json` | `tracked-evidence.json` |
|---|---|---|
| 200 run 1 | `8ae001d61f0cb61e79f7f4f5a3b7608db7dfe19715fa461df9f928877181c461` | `9572aa949ab77ce54ac74704d0fe6e6fbbb02d214d2eb358a96eea63c099dc31` |
| 436 run 1 | `152e87ad8a4aebe2eac835158b30b268915681fe840ee72ca5171b944c8b783f` | `b7109f85aa8905ff867a88749fdd8091a14e01e8939dbda3d7d7be47bd0e22b7` |
| 200 run 2 | `c1adbf54722332a250f24940cf9ea47fdeb66ade2cd5a34c57928cc4066c132d` | `2fccd2efdb9d62713a16e65f51776921fca52b50245bd7cdfa5271c5b47b6a49` |
| 436 run 2 | `54c8eb98964414b0116816bbc79ef0a0915caf53fe754fa85946918fed65dd25` | `28e87d232275d63d9176051b6aae080a7956ee502c753f7412bbb774fb68c353` |
| 200 telemetry off | `3a49715fb33492d9f14fad3add7ada6a7b16b6955688b27961ad794aa6f0838f` | `7f8d33fea63922c9144025eb11dfabb4bae6f4fc01d8b3017414a84b66d5263f` (legacy schema) |

The corresponding whole-run directory identities are
`c28430fc4f62ea1845634f2d7a43a8df7f084b31c7c1b4a750f138a8b6fdb98e`
and `df20ef0c396b08419c8039157a9a152f57b256951c3a7473725ade3a491ec178`
for the two 200-pair runs, and
`c0147eff0d96002c1fd7a19332eb798cae75f49f0732b89a5b4cfe8e23974e32`
and `c72305ca9938bb28c7466126350ba3686edf431ecf4eb635bd79ac85fd97737a`
for the two 436-pair runs. The telemetry-off whole-run identity is
`4b39c97e1363a6f9fa976f6d269c600da8cb58c7b973fdcbb74e1b2fbf9cc86c`.

## Copy-ready Splats handoff

ColmapKit tested one isolated tracked-pose pair-cap expansion on the exact
104-JPEG retained fixture. The opt-in telemetry commit is
`8c34f6251ee031de17fdf8222806e8d72ed582c8`; it adds only
`COLMAPKIT_TRACKED_POSE_FLAG_V2_PAIR_GRAPH_TELEMETRY` in the existing flags
field and preserves config/result sizes 160/1,208. The baseline proves the
200-pair lexicographic cap is the first graph-coverage loss: 455 candidates are
generated, but the prefix leaves 50 images isolated. The sole predeclared
candidate, cap 436, connects all 104 images and grows geometry to 11,824 points
and 40,849 observations, but deterministically yields a final mean reprojection
error of `1.8899142887e150` with three point errors above `1e100`. Decision:
reject. Do not consume an artifact, change the product cap, or run frozen Brush
training from this candidate. Remove matrix B; it is invalid and must not be
retained as a consumer candidate. Speedy 2 may proceed with Brush correctness
isolation only. No XCFramework was built, no physical iPad run was performed,
and no Splats/BrushKit files were edited.

The smallest evidence-derived next reconstruction question is whether replacing
lexicographic cap truncation with one deterministic connectivity-aware pair
selection policy can connect the graph without admitting the pathological
three-track BA tail. That question is explicitly unstarted and needs separate
authorization; it is not another cap test and is not part of this result.

## Publication and remaining gates

No artifact was packaged because the candidate failed the geometry gate. No
physical-device time was used. Nothing was pushed, published, tagged, released,
or pinned. The current product and published controls remain unchanged.
