# ColmapKit FrameFeatureImportV1 owner handoff

## Scope and atomic output

`FrameFeatureImportV1` is an additive CaptureSeal operation. It consumes one
ordered, immutable sealed set of FrameFeatureExtractionV1 `.ckfeatures`
artifacts and their original JPEGs. It deterministically assigns camera and
image IDs and writes their cameras, images, keypoints, and SIFT descriptors to
a COLMAP database.

The authoritative output is one no-overwrite `.ckseal` directory:

```text
capture.ckseal/
  database.db
  import-receipt.json
```

The database and receipt are built and closed in a private sibling directory.
One platform no-replace directory rename publishes both together. A failure,
cancellation, input change, or destination race removes private staging and
leaves no importer-owned visible output. This is why the API does not accept
independent database and receipt destinations: two paths cannot be committed as
one filesystem object.

This slice performs no extraction, fallback, matching, reconstruction, Metal
work, workflow scheduling, or package/device integration. The released v0.2.1
sparse ABI and the existing tracked-pose and extraction ABI structures are
unchanged.

## C ABI and ownership

The separate operation family is:

```c
ColmapKitStartFrameFeatureImportV1(...);
ColmapKitCancelFrameFeatureImportV1(...);
ColmapKitWaitFrameFeatureImportV1(...);
ColmapKitReleaseFrameFeatureImportJobV1(...);
```

`ColmapKitFrameFeatureImportItemV1`, config, progress, and result structures
begin with `struct_size` and `abi_version`. Version 1 requires the complete
version-1 item/config layouts and zero reserved fields. Result and error writes
remain bounded by the caller-provided size. The caller retains callback user
data until wait or release completes; all paths, names, hashes, items, and
extractor configuration are copied before `Start` returns.

The operation owns one worker. It accepts only the already-defined canonical
CPU, one-worker FrameFeatureExtractionV1 profile. There is no Metal identity and
no CPU fallback. Callers provide explicit aggregate bounds for artifacts,
images, features, and an optional base database. The result records admitted
memory, sampled process RSS on Apple platforms, validation/database/receipt and
total clocks, source/profile identity, counts, and receipt/database/sealed-set
hashes.

All filesystem input and output paths must be absolute, and the output parent
directory must already exist. Sealed image names must be unique relative POSIX
paths with no empty, dot, parent, absolute, or backslash components.

## Validation and deterministic assignment

Before database creation, every item must pass the extraction artifact parser:

- exact artifact schema/ABI, completion marker, payload and whole-file hashes;
- exact stable frame ID, revision, image bytes, metadata, extractor profile,
  backend, and engine source identity;
- valid canonical orientation, dimensions, camera model/parameters, finite
  six-column keypoints, typed row-major SIFT-128 bytes, and consistent counts;
- no duplicate image name, stable frame identity, or artifact path;
- no missing, stale, corrupt, truncated, unknown-version, partial, mixed-source,
  mixed-profile, mixed-backend, or over-budget item.

The declared item order is the seal order. Image IDs are `orderIndex + 1`.
Camera IDs begin at one and are assigned at the first occurrence of each exact
canonical metadata SHA-256; later items with that identity share the camera.
No matching rows are created. The receipt records the mapping and each source
artifact/image/metadata hash.

The importer rechecks image and artifact bytes immediately before publication.
Edits, deletions, replacement files, and configuration/source drift therefore
fail closed rather than falling back to live extraction. Reordered input is a
different sealed set and produces a different order/mapping receipt. Duplicate
capture events must be reconciled by the workflow into one declared frame
identity before import.

## Database modes and transaction boundary

`CREATE_NEW` creates the COLMAP schema in private staging. `COPY_EMPTY_BASE`
first requires an absolute, regular, closed, sidecar-free base database under
the declared byte bound. The base is copied privately and its bytes are checked
for change during the copy. Every COLMAP table must be empty, including rigs,
cameras, frames, images, pose priors, features, descriptors, matches, and
verified geometries. Nonempty or incompatible bases are rejected; the base is
never mutated.

All camera/image/keypoint/descriptor writes use one SQLite transaction in the
private database. COLMAP's database transaction wrapper commits when the scope
ends and exposes no rollback operation, but partial commits remain private. On
any later error or cancellation, the entire staging directory is removed. Thus
the public transaction boundary is the exclusive `.ckseal` directory publish,
not visibility of intermediate SQLite pages.

Cancellation is observed between item validations and database writes, before
receipt generation, and immediately before publication. A blocking SQLite
operation is not interruptible. Release requests cancellation and joins the
worker. Foreign callback exceptions are contained at the C boundary.

## Receipt version 1

`import-receipt.json` is deterministic and contains no timestamps, elapsed
times, absolute paths, or process-specific values. It records:

- schema/ABI and import mode;
- exact engine source identity, CPU backend, extraction profile SHA-256, and
  optional base-database SHA-256;
- sealed-set and pre-publication database SHA-256;
- camera/image/keypoint/descriptor counts;
- for each ordered item: stable ID/revision, image name, assigned camera/image
  IDs, image/metadata/artifact hashes, and feature/descriptor counts.

Same-machine repeated imports require identical receipt bytes, sealed-set hash,
and importer-recorded database hash. Reopening SQLite can update database header
bookkeeping, so consumers must validate the recorded checksum before opening the
database and must not mistake a later post-open file hash for the importer
boundary.

## Local proof and remaining gates

`colmapkit/frame_feature_import_test` covers released sparse-layout invariants,
new ABI rejection, ordered IDs and camera reuse, feature row integrity,
same-machine repeat identity, cancellation after a staged row, staging cleanup,
duplicate/mixed/stale rejection, destination preservation and publication
races, successful empty-base copying, and nonempty-base rejection without base
mutation. The existing extraction, sparse-progress, and tracked-pose tests remain
the compatibility controls.

This source-only slice does not prove cross-device database/artifact identity,
crash-durability under sudden power loss, Swift ergonomics, Simulator or
physical-device runtime, App Store/XCFramework packaging, or workflow-level
capture reconciliation. A consumer must keep the `.ckseal` directory immutable,
verify its receipt before opening SQLite, and separately invalidate it when any
declared image, frame revision, order, extraction profile, or engine identity
changes.
