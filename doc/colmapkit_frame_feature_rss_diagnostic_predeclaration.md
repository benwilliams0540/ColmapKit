# ColmapKit FrameFeatureExtractionV1 RSS diagnostic predeclaration

Status: predeclared before execution. This is a diagnostic of the immutable
RC2 bytes, not an RC2 rerun and not a repair candidate.

## Frozen authority

- Owner source/HEAD before diagnostics:
  `91a2a41e386352e6793b4104de264e63c28cf6c7`
- RC2 source: `670a14bb71c5d543769251e608bcfacbef85fdd4`
- RC2 runtime: `0.3.0-rc.2+670a14bb`
- RC2 ZIP SHA-256:
  `2ff308666327193012cb9b6d2974d3d58ecb3e1b2714e885bdb52c3e8cf2bf70`
- RC2 macOS framework and all RC1/RC2 packages, attempts, receipts, and
  result documents remain byte-for-byte unchanged.
- Fixture: committed 1024 x 768 `frame-01.jpg`, SHA-256
  `2b22ced55baccd6493e49d8b5086367c34c952ec63399ea5e96c3dd52ace1767`.
- Extraction profile remains CPU, one worker, 1024 maximum image dimension,
  4096 terminal rows, first octave -1, four octaves, three levels per octave,
  two orientations, L1-root normalization, no fallback, and the exact RC2
  thresholds and 320 MiB admission budget.

## Preserved evidence and causal hypothesis

RC1 ended its first completed extraction at 310,345,728 resident bytes from a
56,770,560-byte start: 253,575,168 bytes of growth. RC2 ended the complete
matrix at 314,081,280 bytes from a 56,852,480-byte start: 257,228,800 bytes of
growth. The full matrix therefore retained only 3,653,632 bytes more than the
first-job RC1 boundary, which argues against per-job accumulation.

For 1024 x 768 and first octave -1, VLFeat allocates a 2048 x 1536 float
workspace. Its fixed `temp`, `octave`, `dog`, and `grad` buffers have
multipliers 1 + 6 + 5 + 10 = 22, totaling exactly 276,824,064 bytes. The
diagnostic tests whether those buffers are freed as live malloc allocations
but remain resident as allocator high-water pages.

## Diagnostic and decision rules

The probe is a fresh C++ process linked to the exact RC2 macOS framework. It
does not use Swift, Foundation, an autorelease pool, a device, sleep, garbage
collection, malloc pressure relief, or a changed engine. It performs two
sequential exact extractions and validations. It records, at process/input,
progress, wait, validation, job-release, extractor-release, and input-release
boundaries:

- `mach_task_basic_info.resident_size`;
- `TASK_VM_INFO` resident, peak resident, physical footprint, internal,
  reusable, compressed, and region count;
- aggregate `malloc_zone_statistics` blocks in use, bytes in use, maximum
  bytes in use, and bytes allocated;
- API status, backend, worker, no-fallback, feature/descriptor counts,
  admission, peak RSS, timings, and artifact identities.

Interpretation is predeclared:

1. If post-release malloc bytes in use remain more than 32 MiB above the
   process/input baseline, treat the first boundary as a live owner allocation
   or harness/reference lifetime and localize it before proposing a repair.
2. If malloc bytes in use return within 32 MiB of baseline, the second run adds
   no more than 16 MiB terminal resident growth, but terminal resident growth
   remains above the unchanged 128 MiB gate, classify the defect as a one-time
   source-owned allocator-resident SIFT workspace.
3. If the pure C++ process returns within the 128 MiB gate while the immutable
   device evidence did not, classify the remaining boundary as device harness
   or platform measurement semantics; do not change engine allocation.
4. If evidence does not isolate one of these boundaries, stop without a
   repair. No sleep, pressure-relief call, gate change, profile change, or
   second diagnostic variant is permitted.

Only after the diagnosis isolates a general boundary may one smallest repair
be predeclared. Extraction math, row counts, ordering, defaults, ABI, artifact
schema, and the 128 MiB terminal-growth gate remain frozen.
