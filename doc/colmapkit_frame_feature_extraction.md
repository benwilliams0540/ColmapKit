# ColmapKit FrameFeatureExtractionV1 owner handoff

## Scope

`FrameFeatureExtractionV1` is an additive ColmapKit operation that extracts
canonical non-covariant CPU SIFT features from one immutable, canonical-upright
JPEG. It emits one self-contained `.ckfeatures` artifact without assigning a
COLMAP image or database identifier.

This first slice deliberately excludes Metal, fallback, matching, database
import, capture scheduling, reconstruction, and tracked-pose behavior. It is
not a package or release decision.

The released v0.2.1 sparse ABI remains unchanged. Its config and result prefixes
still end at byte 136 and byte 1,088; the new operation is a separate family of
symbols and structures.

## C ABI

Every new value structure begins with `struct_size` and `abi_version`. Callers
set both, zero reserved fields, retain callback user data until job completion,
and release jobs and contexts with the matching functions.

The public operation is:

```c
ColmapKitCreateFrameFeatureExtractorV1(...);
ColmapKitStartFrameFeatureExtractionV1(...);
ColmapKitCancelFrameFeatureExtractionV1(...);
ColmapKitWaitFrameFeatureExtractionV1(...);
ColmapKitReleaseFrameFeatureJobV1(...);
ColmapKitReleaseFrameFeatureExtractorV1(...);
ColmapKitValidateFrameFeatureArtifactV1(...);
```

The context accepts only backend `CPU` and exactly one worker. It admits only
one active job and deep-copies the encoded bytes, expected image hash, metadata,
and output path before `Start` returns. The callback and its user data are
borrowed until `Wait` or job release completes.

Concurrent `Start` calls on one context are serialized and all but one are
rejected as busy. Validation may run while extraction is active because context
configuration and provenance are immutable. `Cancel` is atomic and may race the
worker. A caller must serialize `Wait` and release operations for the same job;
concurrent lifecycle mutation of one opaque handle is outside the contract.

Inputs are JPEG bytes, a lowercase SHA-256 of those exact bytes, stable frame
identity and revision, encoded dimensions, orientation `UP`, and one of the
existing PINHOLE, SIMPLE_PINHOLE, or OPENCV camera metadata shapes. Inactive
camera parameters and all reserved fields must be canonical zeroes.

The result reports the actual CPU backend, no-fallback proof, effective worker
count, encoded and processed dimensions, feature and descriptor counts,
admission estimate, sampled process RSS, substage and total clocks, image,
metadata, profile, payload, and whole-artifact SHA-256 values, source identity,
status, and message.

## Lifecycle and cancellation

The context state machine is:

```text
idle --Start--> active --worker completion--> idle
                    | Cancel
                    v
              cancellation requested
```

Cancellation is observed at deterministic safe boundaries around identity
validation, decode, grayscale/resize, the blocking CPU SIFT call,
serialization, and atomic finalization. The CPU SIFT kernel itself is not
interruptible. A request made during it is honored immediately after the call,
before serialization. A request that wins before atomic finalization leaves no
authoritative artifact. Job release requests cancellation and joins the worker.

Progress stages are queued, validating, decoding, preprocessing, extracting,
serializing, and one terminal finished, failed, or cancelled stage. Foreign C
callbacks must not throw; an accidental C++ exception is contained at the ABI
boundary.

The configured memory admission estimate covers two encoded copies, a bounded
image/SIFT working-set estimate, bounded features, and fixed overhead. It is an
admission bound, not a promise that process RSS will equal the estimate. The
result separately samples process resident memory on Apple platforms.

## `.ckfeatures` schema version 1

All integers and IEEE-754 values are little-endian. Strings are a `uint32`
byte count followed by un-terminated UTF-8 bytes. The payload is ordered as:

1. eight-byte `CKFEAT1\0` magic, schema version, ABI version, and complete flag;
2. stable frame ID and revision;
3. encoded and processed dimensions, canonical orientation, camera model,
   active parameter count, and eight camera parameter slots;
4. CPU backend, SIFT extractor type, normalization, and the complete extraction
   configuration that changes feature bytes;
5. keypoint column count (six), descriptor dimension (128), descriptor scalar
   type (`uint8`), feature count, and descriptor byte count;
6. exact image, canonical metadata, extractor profile, and engine source
   identities;
7. ordered keypoints as `x, y, a11, a12, a21, a22` float32 values, followed by
   row-major SIFT-128 descriptor bytes;
8. lowercase SHA-256 of every prior payload byte and the eight-byte
   `CKDONE1\0` completion marker.

The writer creates a sibling temporary file, fully closes it, rechecks
cancellation and destination nonexistence, then uses a no-overwrite hard-link
commit and removes the temporary name. It rejects a preexisting or concurrently
appearing destination and never deletes a destination it did not create.

The validator rejects truncated data, bad magic or completion, unknown schema
or ABI versions, invalid counts or formats, nonfinite keypoints/camera values,
noncanonical metadata, payload corruption, different frame/revision/image or
metadata identity, different extractor config, and different engine source
identity.

## Determinism and invalidation

Within one machine and source/config identity, the CPU path uses stable input
ordering and serializes no timings, paths, database IDs, or other run-dependent
values. Focused tests require repeat outputs to be byte-identical.

Cross-device byte determinism is not yet proven. Consumers must key reuse on all
of these values and reject rather than mix artifacts when any changes:

- encoded image byte SHA-256, stable frame ID, or frame revision;
- dimensions, orientation normalization, camera model, or camera parameters;
- extractor configuration/profile or engine source identity;
- artifact schema or ABI version.

A deleted frame deletes its unreferenced artifact. A frame edit creates a new
revision and artifact; it does not overwrite the old path. Duplicate commit
events may validate and reuse the exact same identity. Capture ordering is not
part of extraction identity and is assigned only when sealing. Interrupted
capture may leave a non-authoritative temporary name, never a valid completed
artifact. Seal-time reconciliation and database import remain unimplemented.

## Local proof and remaining gates

`colmapkit/frame_feature_extraction_test` covers ABI and backend rejection,
successful CPU extraction, same-machine repeat identity, corruption,
truncation, unknown version, configuration and expectation mismatch,
nonfinite data, bounded admission, cancellation and temporary cleanup,
single-job admission, zero-feature failure, and preexisting-output preservation.

This source slice does not prove cross-device byte identity, Simulator or
physical-device runtime, App Store packaging, Swift ergonomics, capture-time
resource policy, or seal-time database reconciliation. The smallest next slice
is a read-only importer/reconciler that validates all identities before assigning
deterministic database image IDs; it must not silently re-extract or mix
profiles.
