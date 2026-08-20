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

## Implementation and exact-source runs

- Diagnostic implementation: `c38323ef6bd43fc2fa868d326298f38738e8c78e`
- Predeclaration: `f67cb69516ddb8c78393336817e9e13535c560cc`
- Candidate implementation: `9efa893625e888422efcc763fec3ba24e22d732a`
- Branch: `integrate/upstream-main-2026-08`
- Candidate flag:
  `COLMAPKIT_TRACKED_POSE_FLAG_V2_POST_BA_NEGATIVE_DEPTH_FILTER` (`8u`)
- Config/result sizes: 160/1,208 bytes
- Exact-source runner SHA-256:
  `f2b1fd04fda8a6b901e411bd75b64b751c9b38c3b365eed1ed690e4c12b2d471`
- Exact-source focused-test SHA-256:
  `de1f479c80a14ce80f14a8b0f374222316ddf469c8925cf47fcbc292e2cc2a9d`

The balanced record consists of two diagnostic controls from the diagnostic
commit followed by two candidate runs from the candidate commit. A final
candidate-source control with both the diagnostic and repair flags absent
confirms the default-off path still produces the same 9,775 points, 29,842
observations, and catastrophic tail. No additional candidate was run.

| Metric | Diagnostic control 1 | Diagnostic control 2 | Candidate 1 | Candidate 2 |
|---|---:|---:|---:|---:|
| Selected / verified pairs | 200 / 199 | 200 / 199 | 200 / 199 | 200 / 199 |
| Registered images | 104 | 104 | 104 | 104 |
| Pre-filter points / observations | 9,775 / 29,842 | 9,775 / 29,842 | 9,775 / 29,842 | 9,775 / 29,842 |
| Final points / observations | 9,775 / 29,842 | 9,775 / 29,842 | 9,774 / 29,840 | 9,774 / 29,840 |
| Final observations per point | 3.05289 | 3.05289 | 3.052998 | 3.052998 |
| Final mean reprojection error | `1.3716427549813397e150` | same | 1.748910525435750 | 1.748910525435743 |
| Final p99 / max point error | 7.098863 / `1.3407807929942596e154` | 7.098863 / same | 7.043724 / 43.785172 | 7.043724 / 43.785172 |
| Nonpositive-depth observations | 2 | 2 | 0 | 0 |
| Filter API count / actual observation delta | n/a | n/a | 1 / 2 | 1 / 2 |
| Matching seconds | 0.319485 | 0.318794 | 0.306505 | 0.306660 |
| Total seconds | 10.300108 | 10.428092 | 10.258667 | 10.098915 |
| Peak resident bytes | 379,043,840 | 393,134,080 | 391,675,904 | 400,195,584 |

The candidate deletes only point 8,557 and its two observations. It clears
cheirality, finite-error, catastrophic-tail, error-quality, point/observation
count, workload-deletion, matching, and wall-clock gates. Its exact selected
pair list remains
`ca94102b0c4e3b20182ce9656354d471c309234988ed8ec21380f9890089dac6`.
All discrete metrics are identical and floating metrics agree far inside
`1e-9` relative tolerance.

It fails three predeclared gates:

1. observations per point remain 3.052998, below the 3.360419 geometry floor;
2. both diagnostic-enabled candidate runs exceed the 374,896,640-byte RSS
   ceiling; and
3. sparse-model and refined-pose hashes differ between candidate repeats.

The final candidate-source control with diagnostics and repair disabled uses
365,084,672 peak bytes, but it is not a candidate measurement and cannot be
used after the fact to waive the diagnostic-enabled gate. Its result confirms
that the default-off candidate source retains the prior invalid output rather
than silently enabling the repair.

Decision: **reject `post_ba_negative_depth_filter_v1` for consumer
integration under the declared gates**. The operation is a narrow and
effective correctness repair, but this goal does not retain candidates by
discarding failed resource or byte-repeatability gates. Do not package, pin,
device-test, or train from it.

## Evidence identities

Evidence root:

`/Users/brw/Developer/ai-projects/colmap/build/codex-artifacts/tracked-pose-validity-boundary`

| Run | Whole-run manifest SHA-256 | `points3D.bin` | Refined poses | Evidence JSON | Runner result |
|---|---|---|---|---|---|
| Diagnostic control 1 | `936c36e859e095ebcef2e8219ce44a92c2aa94dd87b96c320ea5e2f7595b1595` | `4972bc0a613df9056dedc00da833beb4b8adf749c73815c63dbb98f817db04be` | `2de728329ca99d552344a22da4ecad132ebfd9b4d2c81b67e604137c8280c54b` | `c767046eb12081ff9bd03e87f6f6bfab6747dfa895b4ee022ecd072e89c20f20` | `350d1f6159f8f4e2b94082a5a030c8832986a5d970cae05a514bf5dbc702074b` |
| Diagnostic control 2 | `7b8e1cc72180676c6975c12e78d5ac1abfb6bdba50a868e862e5695d35fc1297` | `80ee8540a71e7f485da3e1d96ebf8f939255a60089e811092d40475460e98936` | `62193824b84991c2d9752fe0cea6a0ddafb50fe07335e7ace254c42833963e46` | `762cb6595e7e70198aa6dfbb1415cf2d470fbde7d5c8075328d30fc263642825` | `1a17558bc7ee12e6af8df6107fb1a4d872175bf4c41ca7422ae7d42a6ec3acde` |
| Candidate 1 | `3818cbefa5944737e83f9085a3664e0464863dfa8dd2b8ca14c8654d09cda511` | `00978e718c425d25698cc33039d8a9d97d458c27965fbbfb56a0e83f2a707189` | `2d501231247fc8798b744e47134a4c34c435d00e866648f9ef7ba0d7f7b74d1d` | `9c4202a32f9531233ce8f9c510fb10dbc8aa734c779bbe225cb4f45dedbe1b8f` | `0c2564ac109aec5e066ba597cda24a194e1cca9c6378af17cc8f7dbb89fa53c6` |
| Candidate 2 | `35f396b820877bb2b1bf2598d661d8b4b806d94bb203d74b6f5eab6c51143d29` | `d0ae903ba91c51b0334beeee9141ad1cb0916b9bdc646ece11021c7cf4fd7aad` | `15c27319fc98aa191cbf3bec2ba81db830b6ada10f3f06f3d3ff7a214b35a942` | `ffde27c1b36ab36cb9d3ed2b69faebc519b7bc1f9b5b1f98e69c4e247c864d06` | `87f44c0995a09a2f64fdd3c9d163b26c6cb07d899c80ab1f357f7431f7d1b0c2` |
| Final-source default off | `f0a432efe771e4d621ad3b1eb7c8ea3f9229206a987d609d8d85234609a0173c` | `39516fccda2005d91dbc2d39a52b04311ee56ac8ad4ec4dd4c8d949dcc81e1a5` | `8932e0fa6a322811c34604b4a02069f752b9437e74dbdd347196b6c3fb01f7f9` | `6c99ef0890475d2f2553fda157ccce4e2140e61195b69b7f774581c3a7c093c4` | `728ecf9666b0abe99349f405e4b71f16153bad3e74e8b399c9f7ad4b65e99806` |

The capture manifest SHA-256 is
`0ab979b4214b86a1aeb04f680f2e06040707123694c7bacd6309a073398983b6`;
the previously verified name/size/content image-set identity remains
`1171459bec06367752f4ebcd16e63c1afdc8ec5d055b35fde2ea8e633c24567c`.

Exact candidate execution form:

```bash
build-colmapkit-v2-dev/src/colmap/colmapkit/colmapkit_tracked_capture_runner \
  CAPTURE_MANIFEST OUTPUT_DIRECTORY 200 1 1 1 1
```

The final four arguments enable pair telemetry, the already-rejected
connectivity selector used only as the frozen diagnostic trigger, geometry
diagnostics, and the sole post-BA filter respectively.

## Handoff and next boundary

Speedy 2 must not integrate this candidate or create a frozen-600 lane. No
package or reconstruction bundle is a consumer candidate. The direct app
thread connector is unavailable in this environment, so the committed
machine-readable owner receipt is
`/Users/brw/Developer/ai-projects/colmap/doc/colmapkit_tracked_validity_owner_handoff.json`.

The newly narrowed, unstarted boundary is deterministic four-thread BA output
and a production-representative resource proof that does not retain full
in-memory diagnostic snapshots. No further pair, track, filter, frame, or
training rule is justified by this experiment. A new task would need to decide
whether exact binary repeatability remains mandatory before investigating that
boundary.

No package, physical device, Metal route, depth, fallback, Brush training,
Splats or BrushKit edit, push, PR, remote CI, tag, release, publication, repin,
or product-default change occurred.
