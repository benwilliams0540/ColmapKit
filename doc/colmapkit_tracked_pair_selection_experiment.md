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
