# Simple Snapshot Demo

Application modules and static composition tests for Demo 006.

The current tract is:

```text
WalCore::head
  -> PersistenceSlider -> DurableF
  -> HashChainSlider -> HashF
  -> BitAccumulatorSlider -> BitF
  -> composition reclaimer -> WalCore::tail
```

`HashChainModule` maintains a deterministic 64-bit FNV-1a-style chain over the
previous digest, absolute position, physical sequence, payload size, and every
payload byte. It is an application consistency state, not a cryptographic
authenticator.

`BitAccumulatorModule` maintains the total number of set payload bits and an
order-sensitive rolling fold over record identity and payload. Both modules
require the next absolute position exactly, process synchronously without
allocation, and become fail-closed on a gap, duplicate, or reordered position.

Both sliders use `AvailableRangeAcquire` and `OnePositionPublish`. Only the
composition advances `tail`, and it follows `BitF`, the last mandatory stage.

The fixed 64-byte application payload distinguishes `Data` and
`SaveSnapshot`. A snapshot generation is identified by the absolute position
of its `SaveSnapshot` record. Each stateful module first applies its ordinary
transition, reaches `StateAfter(position)`, and then stores one immutable
capture. An occupied capture slot makes a later snapshot record retryable and
leaves the module state and slider frontier unchanged.

`CaptureGenerationCoordinator` belongs to the composition. It publishes an
in-memory complete generation only when both captures have matching generation,
record position, exclusive processed end, and physical sequence. Module slots
remain occupied until the composition releases the complete generation.

`SnapshotSink` serializes canonical little-endian module files plus a checksummed
description containing WAL, epoch, manifest, composition, generation, sequence,
module schema, size, and checksum identities. It writes and synchronizes a
`snapshot-N.pending` directory, creates `snapshot.description` after both module
files, then atomically renames the directory to `snapshot-N`. Failed saves keep
the coordinator and both module capture slots intact; a retry removes the stale
staging directory. A successfully published generation is never overwritten.

`SnapshotLoader` reads a published generation into an isolated
`PreparedSnapshot`. It validates the description CRC, expected WAL and
composition identity, generation boundary, required module identities and
schemas, exact sizes, file checksums, module encodings, and matching capture
boundaries before exposing prepared state. `restore_snapshot_quiescent()` then
publishes both module states together and initializes both stateful sliders and
frontiers to the exclusive resume position `N + 1`. Loading failures leave the
existing composition untouched.

Negative bootstrap coverage also rebuilds valid outer checksums around
cross-boundary captures and malformed module encodings, proving that validation
does not rely on CRC alone. A replay source that repeats `N` or skips `N + 1`
is rejected by the first stateful module without advancing either frontier.
