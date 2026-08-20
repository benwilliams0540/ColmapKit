# ColmapKit tracked-pose connectivity-aware pair selection

Date: 2026-08-19

## Pre-run declaration

This is one bounded experiment on current integrated ColmapKit source. It does
not reopen the rejected 436-pair cap experiment. Control and candidate both
select exactly 200 pairs from the same 455 generated candidates and freeze all
other reconstruction dimensions documented in
`colmapkit_tracked_pair_graph_experiment.md`.

### Rejected-tail localization

The three 436-pair catastrophic points are 10,302, 10,400, and 10,532. Each
has a three-image track over frames 87, 88, and 89. Their supporting selected
edges are temporal deltas one and two, not revisit edges:

- frame 87--88: 265 verified inliers;
- frame 88--89: 130 verified inliers;
- frame 87--89: 119 verified inliers.

Points 10,302 and 10,400 use distinct feature indices at exactly identical
image coordinates and finish at identical 3D coordinates. Point 10,532 is a
second three-view ambiguity. After BA, one observation of the duplicate point
has near-zero signed depth. The three stored point errors are above `1e153`.
The pair inlier counts are not weak, all values before BA are finite, camera
corrections stay bounded, and Ceres calls the solution usable. The evidence
therefore rules out revisit ranking, pair-cap cost, silent fallback, and an
obvious low-inlier pair as the first cause. It localizes the failure to an
ambiguous temporal track/merge configuration becoming pathological at the BA
boundary. It does not distinguish the last responsible operation between
track merging and BA without changing another frozen dimension.

No frame ID, feature index, or output-derived exclusion is allowed in the
candidate.

### Sole selection policy

Policy identity:
`connectivity_backbone_temporal_baseline_v1`.

The existing temporal/revisit candidate generator is unchanged. Before any
matching, annotate each generated edge with its already-computed class,
capture-order separation, pose-center distance, and pose rotation difference.
Rank edges deterministically as follows:

1. temporal before revisit;
2. temporal edges by decreasing capture-order separation, using the larger
   baseline inside the already-bounded three-neighbor window as the
   triangulation-quality proxy;
3. revisit edges by increasing rotation difference and then increasing the
   existing pose-distance neighbor rank;
4. canonical endpoint order indices as the final tie-break.

Scan that rank with deterministic union-find and retain every edge that joins
two components until all images are connected. Then append the highest-ranked
remaining edges until exactly `max_image_pairs` (200) are selected. Canonically
sort the final set before writing/matching so execution order is stable.

This is a general connectivity backbone plus quality-ranked remainder. It uses
only input order, pose facts, and candidate metadata available before matching.
It does not use inliers, reconstructed tracks, fixture-specific identities, or
a second all-pairs matching pass. The policy remains default-off behind a new
bit in the existing `flags` field; V2 structs must remain 160/1,208 bytes.

### Acceptance gates

The candidate is retained only if both candidate runs clear every gate:

- **Coverage:** exactly 200 selected pairs; selected and verified graphs each
  have one component, zero isolated images, and minimum degree at least one.
- **Catastrophic tail:** no point error above `1e3`; maximum point error no
  greater than twice the control maximum (`215.52994305015542` px); p99 point
  error no greater than 110% of control (`9.687002848022162` px).
- **Reprojection:** final mean reprojection error no greater than 110% of the
  control (`2.429726439227823` px).
- **Geometry:** at least 15% more points and observations than control (at
  least 6,462 points and 22,858 observations), observations per point at least
  95% of control (`3.360419`), and median track length at least 3.
- **Bounded cost:** matching no more than 1.5 times the control median
  (`0.401657` s), reconstruction-call wall no more than 1.15 times control
  (`11.962044` s), and peak RSS no more than 1.25 times control
  (`374,896,640` bytes).
- **Repeatability:** the two candidate runs must have identical pair-list and
  sparse-model/refined-pose hashes and identical discrete/headline geometry
  metrics; floating metrics must agree within `1e-9` relative tolerance.

Passing sparse gates would justify only a default-off reconstruction candidate
for downstream frozen-600 evaluation. It would not establish product quality
or a product speedup. Failure of any gate rejects this policy and ends the goal
without another selector or parameter change.

## Implementation identity

- Pre-run declaration commit:
  `0d549f3b1801040829a9b813645dc2ed74c92b04`
- Default-off selector/telemetry commit:
  `5943d597ca420b1e82083e6a7f4416b9bd8737d7`
- Branch: `integrate/upstream-main-2026-08`
- New existing-field flag:
  `COLMAPKIT_TRACKED_POSE_FLAG_V2_CONNECTIVITY_PAIR_SELECTION` (`2u`)
- Config/result sizes remain 160/1,208 bytes.
- Runner SHA-256:
  `effcdfa616617fedee934615e6a786295bb9b89e8a862c9b1ad81dbe246a44d9`
- Focused-test SHA-256:
  `d496471bc9b9d2dc2f932616fecd41feb09aaf692156834c36686ba46fb53e5e`

With the flag absent, the original lexicographic selector is byte-identical to
the prior current-source control: pair list, cameras, images, frames, points,
rigs, and refined poses all retain their prior hashes. With the flag present,
evidence identifies the policy and reports backbone size, selected edge
classes, and pre-match pose metadata per selected edge.

## Balanced result and decision

The alternating macOS order was control 1, candidate 1, control 2, candidate
2. Both lanes used exact engine source `5943d597ca420b1e82083e6a7f4416b9bd8737d7`,
the retained 104-JPEG image-set SHA-256
`1171459bec06367752f4ebcd16e63c1afdc8ec5d055b35fde2ea8e633c24567c`,
and the frozen configuration above.

| Metric | 200 lexicographic control | 200 connectivity candidate |
|---|---:|---:|
| Selected / raw / verified pairs | 200 / 200 / 200 | 200 / 200 / 199 |
| Verified inliers | 38,417 | 36,576 |
| Selected graph components / isolated | 51 / 50 | 1 / 0 |
| Verified graph components / isolated | 51 / 50 | 1 / 0 |
| Verified minimum / maximum degree | 0 / 14 | 1 / 4 |
| Registered images | 104 | 104 |
| Sparse points | 5,619 | 9,775 |
| Observations | 19,876 | 29,842 |
| Observations per point | 3.537284 | 3.052890 |
| Median / p99 point error | 1.819181 / 8.806366 px | 1.418402 / 7.098863 px |
| Maximum point error | 107.764972 px | `1.3407807929942596e154` px |
| Final mean reprojection error | 2.208842 px | `1.3716427549813397e150` px |
| Median feature extraction | 8.566183 s | 8.543858 s |
| Median matching | 0.291168 s | 0.316335 s |
| Median triangulation | 0.060105 s | 0.061925 s |
| Median bundle adjustment | 0.520289 s | 0.258492 s |
| Median export | 0.780340 s | 0.784458 s |
| Median reconstruction-call wall | 10.272065 s | 10.025762 s |
| Median peak RSS | 331,153,408 bytes | 362,086,400 bytes |

The candidate selects a 103-edge backbone plus 97 ranked remainder edges. All
200 are temporal: 101 delta-three and 99 delta-two edges. The same delta-three
edge, frames 52--55, has only eight raw matches and fails geometric
verification in both runs; the verified graph nevertheless remains connected.

The candidate adds 73.96% points and 50.14% observations, but observations per
point fall 13.69% and verified inliers fall 4.79%. More importantly, both runs
contain the same catastrophic point 8,557, with the same stored error above
`1e154`. It is a two-observation track over frames 87 and 88. The direct 87--88
edge is not in the selected set, proving that a transitive correspondence,
completion, or merge can leave a pathological track even when the direct pair
is absent. The candidate's model and refined-pose hashes also differ between
runs despite identical discrete metrics and failure magnitude.

Gate results:

- coverage: pass;
- matching, wall, and RSS cost: pass;
- point-error tail: fail;
- mean reprojection error: fail;
- geometry quality: fail because observations per point are below the floor;
- repeatability: fail because model/refined-pose hashes differ.

Decision: **reject `connectivity_backbone_temporal_baseline_v1`**. Do not
package, integrate, train from, device-test, or promote it. Sparse improvements
and bounded timing do not compensate for a catastrophic BA result.

## Evidence and checksums

Evidence root:

`/Users/brw/Developer/ai-projects/colmap/build/codex-artifacts/tracked-pose-connectivity-selector`

| Run | Pair-list SHA-256 | `runner-result.json` SHA-256 | `tracked-evidence.json` SHA-256 |
|---|---|---|---|
| final control 1 | `ffd922502cc54576c4cbfc88bf819bb7c299f750b704892add687317e93410a8` | `4500fce1643c6a633adfcf39baf6e185c6ae36584e3a1ecec14b3d312488eda7` | `0dd7f3d7b65cb092a6795fb5e9776810ff0c020195884cb0ea53727d098af57f` |
| final candidate 1 | `ca94102b0c4e3b20182ce9656354d471c309234988ed8ec21380f9890089dac6` | `2f37576f8feaa5eb7871da2053b0a43d2671110cb0c63772b69edc886125215c` | `cbf1065ac131cfe705170ee1cabc9769f30eb525a78e0dbf806b22cf710126f5` |
| final control 2 | `ffd922502cc54576c4cbfc88bf819bb7c299f750b704892add687317e93410a8` | `8822e578bd6b253f345cf8696b95ec7d1665bbc80c0ab9af80918361267be155` | `e95ab7b1cc0a473e46f1225984016a0c73f063eb46aea62babe184dc6d03d353` |
| final candidate 2 | `ca94102b0c4e3b20182ce9656354d471c309234988ed8ec21380f9890089dac6` | `9d344fba13d6bf595d697537cbe6b2f84ecac8fee42ae58aecd9964aa22493c2` | `8b666349df9c1cf509b7968a859ee6f159f67439e408e92c469420e8f9098d59` |

Control model/refined-pose hashes are stable and match the earlier experiment:

- `points3D.bin`:
  `f181463b2f108c3f8baa7f3f1a5e451a233939e9f523c16846b705be43db8fb1`
- `refined-poses.json`:
  `70f0d3d4374500477264b4dfc01bef74b6b9b6c7db992f981b2aaf5c6896a093`

Candidate model/refined-pose hashes:

- run 1 `points3D.bin`:
  `4256a0650aec92789501114f4c8bf5e727de86d68debdc715908382c97c9b937`
- run 2 `points3D.bin`:
  `0d629b574e392aa375a683083c8a66ad039a06d81f2a665493ad0d4e8f074272`
- run 1 refined poses:
  `a202eb1548d113e68d26fc64b6878f5a3f9d03793eb4b6759cfac9f1814ead58`
- run 2 refined poses:
  `c7f2e961b8db595f87e5eda9d2df764f5985f36bd7c57c5cf40a9ecbf46441c2`

Whole-run path/size/content manifest identities are
`71af73683b48ddba79eee6f81a77bb2551e60f2c35fae6c0eb3a2ddc8e24b654`
and `c39cac4181d161ed68798bc8159a796f1a87ab6cc5901cd0d26f09ad007effb5`
for controls, and
`f88bf5b9cf356ba9cf9d91a09efc771f79286a445f25524f14e06d3537a52672`
and `7f6dc160934043e3c02a62a02d5ea9f27573e66ce8c8d9f67b54861cb76b365d`
for candidates.

The earlier `control-200-*` and `candidate-200-*` directories are retained as
excluded attempts. Their executable source bytes were correct and their
results agree, but the cached full `COLMAPKIT_SOURCE_REVISION` string had a
mistyped suffix. The `final-*` directories above were rebuilt and rerun after
pinning the verified full SHA and are the authoritative record.

Exact execution form:

```bash
cmake -S . -B build-colmapkit-v2-dev \
  -DCOLMAPKIT_SOURCE_REVISION=5943d597ca420b1e82083e6a7f4416b9bd8737d7
cmake --build build-colmapkit-v2-dev \
  --target colmapkit_tracked_capture_runner colmapkit_v2_test -j 4
build-colmapkit-v2-dev/src/colmap/colmapkit/colmapkit_v2_test
build-colmapkit-v2-dev/src/colmap/colmapkit/colmapkit_tracked_capture_runner \
  CAPTURE_MANIFEST OUTPUT_DIR 200 1 CONNECTIVITY_POLICY
```

`CONNECTIVITY_POLICY` was `0` for controls and `1` for candidates.

## Remaining boundary and downstream action

The first remaining reconstruction boundary is not pair count or graph
connectivity. It is deterministic validation/handling of ambiguous transitive
tracks across triangulation completion/merge and the post-BA cheirality/error
boundary. A future task may isolate one general track-validity or post-BA
correctness rule, but that work is unstarted and must not be blended with this
rejected selector.

Speedy 2 must not integrate this policy or create a frozen-600 matrix lane for
it. No package exists. Speedy may continue its independent Brush correctness
isolation on the retained 200-pair control only.

No physical device, Metal route, depth, fallback, Brush training, Splats edit,
package, push, PR, remote CI, tag, release, publication, repin, or product
default change occurred.
