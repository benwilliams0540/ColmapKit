# ColmapKit RC1 320 MiB physical follow-up predeclaration

Status: predeclared before physical launch. This file contains no runtime
result and must not be edited to retrofit a result.

## Authority and one variable

This experiment reuses the exact physically rejected RC1 package and every
fixture, extraction/import option, backend, worker count, correctness gate,
cancellation gate, lifecycle count, RSS limit, and thermal limit from the
base matrix. The only experimental variable is:

```text
memory_admission_budget_bytes:
  control:   268435456 (256 MiB)
  candidate: 335544320 (320 MiB)
```

No second budget, sweep, fallback, resolution/feature reduction, threshold,
algorithm, engine, or package change is permitted. The general selection rule
is to evaluate the existing deterministic estimator for every immutable frame,
take the maximum, and round upward once to the next 16 MiB boundary. The
fixture maximum is 326,582,652 bytes, which selects 320 MiB.

## Immutable identities

- Owner branch: `integrate/upstream-main-2026-08`
- Harness/config commit:
  `7cf1daa72a7106c08883a39fba0645b215c7a51a`
- RC source: `c69711b89848a7ffe0b8933ea8636f87be8c1c50`
- Runtime: `0.3.0-rc.1+c69711b8`
- ZIP SHA-256 / SwiftPM checksum:
  `e7e69b029715bfe4f63551c05fdc9851ef565844f216669b5e0fbea5ba8a5aa3`
- iPhoneOS binary SHA-256:
  `8876954755d656e1968426d411c24ac48e87d1903ee5f449126f042f6a704eb0`
- Base matrix SHA-256:
  `47071bd7ef9a2a94dab5b991faec97e5f998646049514c1cfd3582262c6f086d`
- Fixture manifest SHA-256:
  `cb1ac79e7592c3bb33903e62ee342596597752f48f7b9f64582ca5102c639e1a`
- Harness Swift SHA-256:
  `7c3a1c7fd6d7302730c394bb94c8541e64083d5ca4e618478189ae5b3b214825`
- Guarded runner SHA-256:
  `e5ada4bdbc66bf11a9ffe94f646162692714ed33002a902c3a698dced543eff1`
- Build-only receipt SHA-256:
  `fb3fbc1e7e725e62c387f00e95c15aca19741829b6090fd4a23574d0cbd1a1bf`
- Build-only Xcode log SHA-256:
  `2dc29806014c3c043d923364c54ccf14b11f7b106f917beb420ba7a37bab7960`

The unique retained physical attempt root is predeclared as:

`/Users/brw/Developer/ai-projects/colmap/dist/colmapkit-v0.3.0-rc1-c69711b8/physical/attempt-03-320mib-full-matrix`

Attempts 1 and 2 and
`doc/colmapkit_frame_feature_rc1_physical_rejection.md` remain immutable.

## Frozen numeric profile

- Backend CPU; one worker; no fallback; no Metal combination.
- JPEG maximum encoded bytes: 16 MiB.
- Admission budget: exactly 335,544,320 bytes.
- Maximum image dimension: 1024.
- Maximum features: 4096.
- First octave -1; four octaves; octave resolution 3.
- Maximum orientations 2; `upright=false`; L1-root normalization.
- Peak threshold `0.006666666666666667`; edge threshold `10.0`.
- Three immutable ordered fixture frames with the manifest and image hashes
  committed at the harness/config commit.
- Import mode CREATE_NEW, one worker, 64 MiB artifact bound, 64 MiB image
  bound, 100,000 feature bound, and 64 MiB base-database bound.

## Unchanged execution and retain gates

Run the complete base matrix: repeated extraction and exact validation,
terminal cancellation/cleanup, two deterministic three-frame transactional
imports, database row/ID/no-match inspection, corrupt/truncated/stale,
preexisting-output, duplicate, partial-set, and mixed-profile rejection,
strict Metal rejection, ten sequential extraction lifecycles, two import
lifecycles, and five-second resource sampling.

Retain only if exact source/package/config identities hold; CPU/one-worker/
no-fallback fields are proven; artifact and import repeats are deterministic;
cancellation reaches terminal `CANCELLED` within five seconds and cleans up;
all invalid/conflict cases fail closed; imported IDs/counts and no-match rows
are correct; every job/context terminates; engine peak RSS is below 768 MiB;
terminal process growth is no more than 128 MiB; start storage is at least
2 GiB and battery at least 20%; thermal never reaches critical and ends no
worse than serious; and boundary/resource receipts are complete.

Failure stops this experiment. Success authorizes only the separately defined
local final-v0.3.0 goal; it does not authorize release, publication, consumer
pin, product policy, or manual-quality claims.
