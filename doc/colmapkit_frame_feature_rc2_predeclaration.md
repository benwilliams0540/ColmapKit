# ColmapKit FrameFeatureExtractionV1 count-contract repair predeclaration

Status: predeclared before source implementation, package construction, or
physical RC2 execution. This record must not be retrofitted to a different
repair after a candidate result is known.

## Preserved rejection and authority

RC1 and physical attempts 1 through 3 remain immutable. Attempt 3 proved that
the 320 MiB admission candidate was sufficient to enter CPU extraction, then
exposed an owner contract defect: the `4096`-feature, two-orientation request
emitted 8,736 aligned keypoint/descriptor rows while its own validator admitted
at most `4096 * 2 = 8192`. RC1 is permanently rejected and its bytes are not
eligible for this repair.

The repair starts from clean owner branch
`integrate/upstream-main-2026-08` at
`725c6e2ef796443d23c76f90c3eef6532ca5e192`. The released v0.2.1 ABI, the
separate sparse and tracked-pose APIs, and all preserved RC1 artifacts and
receipts are controls.

## Contract audit

COLMAP's non-covariant CPU SIFT implementation treats
`SiftExtractionOptions::max_num_features` as a nominal localized-keypoint
selection target. It deliberately retains an entire selected DoG level after
crossing that target. It then emits up to `max_num_orientations` descriptor
rows for each localized keypoint. Consequently, raw CPU extractor output is
not a hard bound on either localized keypoints or oriented rows.

The repository already distinguishes this nominal behavior from an exact
post-extraction feature-row cap in its CPU/Metal comparison tooling. The
unpublished FrameFeatureExtractionV1 artifact parser and admission estimator
also attempted to impose a bounded row contract, but incorrectly used
`max_num_features * max_num_orientations` and the writer did not enforce it.

For FrameFeatureExtractionV1, the public contract is therefore fixed as:

- `max_num_features` is the hard maximum number of terminal, oriented feature
  rows returned, serialized, validated, and imported;
- `max_num_orientations` is the maximum orientation hypotheses generated for
  one localized SIFT keypoint before terminal row selection; it does not
  multiply the public artifact row limit;
- `feature_count` is the exact number of aligned six-float keypoint rows and
  SIFT-128 `uint8` descriptor rows in the artifact and imported database;
- the terminal row bound is inclusive: `1 <= feature_count <=
  max_num_features`;
- schema version, ABI version, structure sizes, field offsets, profile hashing,
  and all existing symbols remain unchanged.

This is a pre-release semantic clarification and repair of a self-inconsistent
new operation, not a change to released v0.2.1 callers or to general COLMAP
feature extraction.

## One repair

After CPU SIFT returns and aligned descriptor shape is verified, but before
result reporting, coordinate rescaling, serialization, or import, apply the
repository's existing deterministic largest-scale feature selection to cap
the aligned keypoint/descriptor payload at `max_num_features`. Exact-bound
payloads remain unchanged. Over-bound payloads retain the largest-scale rows
and preserve their descriptor alignment. No nonfinite clamp, fixture-specific
exception, threshold change, resolution reduction, fallback, or core SIFT
algorithm change is permitted.

The parser must reject `feature_count > max_num_features`. The admission
estimator's terminal feature-storage component must use the same hard row cap:

```text
scale = min(1, max_image_size / max(encoded_width, encoded_height))
pixels = ceil(encoded_width * scale) * ceil(encoded_height * scale)
first_octave_factor = first_octave < 0 ? 4 : 1
image_working_set = pixels * first_octave_factor * 96
terminal_feature_working_set = max_num_features * 256
admitted = encoded_size * 2
         + image_working_set
         + terminal_feature_working_set
         + 16 MiB
```

The image/SIFT term is conservative admission accounting for decode, pyramid,
raw detection, and orientation work; it is not an expected process-RSS value.
The terminal feature term bounds the authoritative aligned payload. Actual
process peak RSS remains separately reported.

## Source acceptance gates

Focused tests must prove all of the following before packaging:

- a deterministic high-texture/two-orientation input that makes raw CPU SIFT
  exceed the requested row cap is published with exactly the requested count;
- exact-bound aligned keypoint/descriptor rows remain unchanged;
- over-bound selection is deterministic, keeps rows aligned, and returns the
  hard terminal count;
- extraction result count, descriptor byte count, serialized header/payload,
  artifact validator, importer/database rows, and receipt counts agree;
- a forged or corrupt artifact above the terminal row bound rejects fail
  closed;
- the estimator uses the terminal cap, rejects one byte below its estimate,
  and admits at the exact estimate;
- same-machine repeat artifacts remain byte-identical;
- released v0.2.1 structure prefixes, exports, bounded writes, cancellation,
  cleanup, no-overwrite transactions, and no-fallback behavior remain intact.

Any failure stops this single repair; no second source fix or policy is
authorized.

## RC2 package gates

Only a clean, committed repair source may produce uniquely named runtime
`0.3.0-rc.2+<source8>`. The candidate must repeat the full macOS arm64,
iPhoneOS arm64, and iOS Simulator arm64 static/package/license/header/module/
export/deployment/linkage/source-identity audit and fresh external SwiftPM
consumer builds. RC1 and every earlier candidate remain untouched.

## Predeclared physical profile and decision gates

The physical matrix will use the same connected M1 iPad, immutable three-frame
synthetic fixture, CPU backend, one worker, no fallback, maximum image size
1024, `max_num_features=4096`, `max_num_orientations=2`, first octave -1,
four octaves, octave resolution 3, L1-root normalization, peak threshold
`0.006666666666666667`, and edge threshold `10.0`.

The repaired estimator will be evaluated for all immutable frames. Its maximum
will be rounded upward once to the next 16 MiB; that single value is the only
physical admission budget. There is no budget sweep. All prior cancellation,
transaction, invalid-input, lifecycle, peak-RSS, terminal-RSS-growth, battery,
storage, and thermal gates remain unchanged. The only corrected count gate is
the documented terminal contract: every successful extraction, validation,
artifact, import, and receipt must agree on `1...4096` rows and exactly 128
descriptor bytes per row.

The full matrix includes repeat extraction/validation, terminal cancellation
and cleanup, two deterministic three-frame imports, corrupt/truncated/stale/
duplicate/mixed/partial/conflict cases, explicit Metal rejection, ten
extraction lifecycles, two import lifecycles, and five-second resource
sampling. Any identity, correctness, fail-closed, lifecycle, resource, or
thermal failure rejects RC2 and stops without another fix or tuning.

RC2 success authorizes only the separately defined final local v0.3.0 goal.
It does not authorize a tag, push, PR, CI, release, publication, public SwiftPM
update, consumer pin, product-default change, or manual-quality claim.
