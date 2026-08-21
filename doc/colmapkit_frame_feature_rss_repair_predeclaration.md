# ColmapKit FrameFeatureExtractionV1 RSS repair predeclaration

Status: predeclared after the immutable RC2 diagnostic and before source
implementation or candidate execution.

## Diagnosis

The exact RC2 macOS framework produced two byte-identical 4096-row artifacts
and released every job/context. From the encoded-input boundary to the first
extractor-release boundary:

- resident growth: 246,890,496 bytes;
- live malloc growth: 206,624 bytes.

After the encoded input was also released, process-start-relative values were:

- resident growth: 254,197,760 bytes;
- live malloc growth: 217,392 bytes.

The second complete extraction added only 131,072 terminal resident bytes.
At the live SIFT boundary malloc bytes in use reached 312,762,976; after job
and context destruction they returned to 30,068,720. This proves the engine
objects free their allocations and rules out per-job growth, retained C
results, Swift/Foundation autorelease behavior, and import/database ownership
as the primary cause. The freed 276,824,064-byte first-octave VLFeat workspace
remains physically resident under the Apple process allocator, violating the
unchanged 128 MiB terminal-growth gate.

Raw evidence is retained under
`build/colmapkit-frame-feature-rss-diagnostic-91a2a41e`. The diagnostic JSON
SHA-256 is
`708271dde6f5a6862ab270d911251f1fa5c5584fffab48d661525d934a334da8`.

## Single repair

On Apple builds only, allocate the four fixed `VlSiftFilt` image workspace
buffers (`temp`, Gaussian octave, DoG, and gradient) with anonymous private
read/write virtual-memory mappings and release those exact mappings with
`munmap` in `vl_sift_delete`. Keep keypoint, Gaussian-kernel, descriptor,
artifact, and all other allocations unchanged. Non-Apple builds retain the
existing VLFeat allocator.

This is a lifecycle/storage repair, not a pressure-relief action. It must not
call `malloc_zone_pressure_relief`, `madvise`, garbage collection, sleep, or a
post-job cleanup heuristic. It must not change SIFT math, dimensions, feature
selection, ordering, thresholds, counts, backend/default behavior, public C
ABI, artifact schema, or profile fields. Mapping failure must return extractor
creation failure without a partial live workspace.

## Quantitative gates

Before RC3 packaging or device use, all must pass:

1. The same two-run macOS diagnostic against repaired source reports CPU,
   one worker, no fallback, 4096 aligned rows, 524,288 descriptor bytes, and
   325,534,076 admitted bytes on both runs.
2. The keypoint/descriptor row bytes are identical to the immutable RC2
   artifact. Provenance/header hashes may change only because source/profile
   identity changes; normalized feature-row identity may not change.
3. Terminal `mach_task_basic_info.resident_size` growth is at most 128 MiB
   from process start; second-run terminal growth is at most 16 MiB; post-
   release live malloc growth is at most 32 MiB.
4. Focused high-texture count-contract, repeat artifact, parser/import,
   cancellation/cleanup, and legacy ABI tests pass. No struct or export changes
   occur.
5. If and only if source/focused gates pass, build one uniquely identified
   `0.3.0-rc.3` three-slice package and repeat the established deployment,
   header/module/export, source identity, linkage, license, archive, signature,
   SwiftPM, external consumer, and v0.2.1-client audits.
6. Run exactly one full physical matrix on the same M1 iPad with the unchanged
   CPU-one-worker 1024/4096/two-orientation profile, recomputed one-round
   admission budget, and every RC2 correctness, cancellation, transaction,
   strict-Metal rejection, lifecycle, 768 MiB peak, 128 MiB terminal-growth,
   and thermal gate unchanged.

If any candidate gate fails, RC3 is rejected with no second source repair,
profile tuning, gate change, or rerun. RC1/RC2 artifacts and receipts remain
immutable and authoritative.
