# ColmapKit tracked-pose post-BA validity experiment

Date: 2026-08-20

## Scope and immutable boundary

This is one diagnostic-first experiment at the remaining tracked-pose
transitive-track/post-bundle-adjustment boundary. It does not reopen the
rejected 436-pair cap or `connectivity_backbone_temporal_baseline_v1`
selection experiments. The retained 104 JPEGs, synchronized ARKit poses and
intrinsics, generated candidates, selected 200-pair connectivity graph,
feature settings, matching thresholds, two triangulation passes, 40 BA
iterations, four CPU threads, no-depth behavior, and strict no-fallback route
remain frozen.

The older physical source identity `32d15c99a0778cc7f1ee94f74cf07338e4b5bf68`
remains a separate historical result. This experiment uses only the current
integrated source line and does not redefine that physical baseline.

## Result-neutral diagnosis

Commit `c38323ef6bd43fc2fa868d326298f38738e8c78e` adds default-off geometry
diagnostics under
`COLMAPKIT_TRACKED_POSE_FLAG_V2_GEOMETRY_DIAGNOSTICS` (`4u`). The flag uses
the existing `flags` field; V2 configuration/result sizes remain 160/1,208
bytes. With the flag absent, no snapshot or evidence work runs and no
reconstruction operation changes.

Two exact-source diagnostic controls are retained at:

- `build/codex-artifacts/tracked-pose-validity-boundary/diagnostic-connectivity-run1`
- `build/codex-artifacts/tracked-pose-validity-boundary/diagnostic-connectivity-run2`

Both reproduce 104 registered images, 199 verified pairs, 9,775 points,
29,842 observations, final mean reprojection error
`1.3716427549813397e150`, and the same catastrophic point 8,557. The selected
pair-list SHA-256 remains
`ca94102b0c4e3b20182ce9656354d471c309234988ed8ec21380f9890089dac6`.

The first invalid state is post-BA cheirality, not track creation, completion,
or merging:

- the two-observation track over frame orders 86 and 87 exists immediately
  after the first image-triangulation pass and is unchanged by both completion
  and merge operations;
- the direct pair and direct feature correspondence are absent, while the two
  observations are reachable within completion transitivity five;
- before BA, every observation has positive finite depth, there are no
  catastrophic errors, and point 8,557 has finite mean error
  `61.747748545531778` pixels;
- after BA, both observations have depth about -30 meters and each receives
  `sqrt(DBL_MAX)`, yielding point error `1.3407807929942596e154`;
- Ceres reports the solution usable, but the tracked route exports it without
  a post-BA cheirality validity pass.

The two runs differ only by tiny post-BA floating values and reproduce the
same structural state, exact bad track, negative-depth count, catastrophic
tail, and discrete metrics. The diagnostic therefore isolates a general
ColmapKit-owned omission: a usable numerical BA solution is not necessarily a
cheirality-valid reconstruction.

## Sole predeclared candidate

Candidate identity: `post_ba_negative_depth_filter_v1`.

Add one default-off flag in the existing `flags` field. When enabled, after a
usable BA solve and before final error computation/export, invoke the existing
COLMAP `ObservationManager::FilterObservationsWithNegativeDepth()` operation.
Record its reported count and the before/after snapshot transition in evidence.
Do not change track construction, merge logic, BA residuals, pose weights,
pair selection, reprojection thresholds, or training. Do not add
`FilterAllPoints3D`, a coordinate clamp, frame/feature exclusions, or another
candidate.

This is narrower than a general post-BA reprojection sweep: it removes only
observations whose optimized geometry violates cheirality. Because COLMAP
deletes an entire point when such an observation belongs to a two-view track,
the geometry transition must report the actual observation/point delta rather
than treating the API return count as the full deletion count.

## Acceptance gates

The candidate is retained only if both candidate runs clear every gate:

- **Frozen structure:** exactly 200 selected pairs, 199 verified pairs, one
  selected/verified graph component, zero isolated images, and unchanged pair
  list hash.
- **Validity:** zero nonpositive or non-finite depths after the candidate pass,
  zero non-finite errors, and no point or observation error above `1e3`.
- **Error quality:** final mean error at most `2.429726439227823` pixels, p99
  point error at most `9.687002848022162` pixels, and maximum point error at
  most `215.52994305015542` pixels.
- **Geometry:** at least 6,462 points and 22,858 observations, observations per
  point at least `3.360419`, and median track length at least three.
- **No workload deletion:** the candidate may delete at most 1% of pre-filter
  observations, must identify every deletion in the geometry transition, and
  may not change matching, triangulation, or BA inputs.
- **Bounded cost:** matching at most `0.401657` seconds, reconstruction-call
  wall at most `11.962044` seconds, and peak RSS at most 374,896,640 bytes.
- **Repeatability:** both runs must have identical pair-list and discrete
  geometry/validity metrics; floating metrics must agree within `1e-9`
  relative tolerance. The prior gate requiring identical sparse-model and
  refined-pose hashes remains in force.
- **Compatibility:** the default-off control remains behavior-identical, V2
  structs remain 160/1,208 bytes, and focused ABI/canary/cancellation tests
  pass.

Passing these gates would retain only a default-off reconstruction candidate
for downstream frozen-600 and physical evaluation. It would not establish
product quality, a product speedup, release readiness, or permission to
package/publish. Failure of any gate rejects the candidate and ends this goal
without a second repair.
