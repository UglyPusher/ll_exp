# WAL Design

## Components

`Wal` owns lifecycle, storage, frontiers, failure state, and the concrete physical
file writer. It exposes only lifecycle, the three role operations, and a
diagnostic snapshot.

`Storage` owns one aligned allocation. It implements exactly four lifecycle and
addressing responsibilities:

```text
initialize/allocate -> warm/touch -> block_at_slot(slot) -> release
```

It does not implement frontier policy or persistence.

`Wal` keeps absolute `tail`, `durable`, and `head` frontiers for ordering and
sequence numbers. It keeps separate slot indexes for block addressing, so the
hot path advances slots with a simple increment-and-wrap instead of deriving a
slot from `position % capacity` on every access.

`PhysicalWalFile` owns the native OS file handle. It creates the file, writes
the file header, serializes physical records with CRC and padding, synchronizes
one completed batch, and closes the handle. It has no virtual interface and no
knowledge of ring frontiers.

## Operation Walkthrough

`try_publish()` validates the call, uses the block at `head`, fills it,
publishes `head + 1`, and returns the derived physical sequence.

`advance_durable()` selects a bounded pending range, passes each immutable block
to `PhysicalWalFile`, requests one physical sync, and publishes the range end as
`durable` only after success.

`try_consume()` uses the readable block at `tail`, copies it to caller memory,
publishes `tail + 1`, and returns the derived physical sequence.

## Frontier Layout

Each frontier is stored in its own explicitly padded 64-byte aligned `Frontier`.
The layout removes false sharing caused by unrelated owners writing `tail`,
`durable`, and `head` in one cache line.

This does not remove legitimate cache traffic: producer acquires `tail`,
durability acquires `head`, and consumer acquires `durable`. The fixed 64-byte
assumption is explicit and may need a portability layer on platforms with a
different destructive-interference size.

## Physical Durability

The physical writer uses native unbuffered application calls rather than C++
iostream buffering:

- Windows: `WriteFile` followed by `FlushFileBuffers`;
- POSIX: `write` followed by `fdatasync`;
- macOS: `write` followed by `fsync`.

One non-empty logical batch receives one physical sync. The durable frontier is a
publication of that completed sync, not of append completion alone.

File creation is exclusive (`CREATE_NEW` on Windows, `O_CREAT | O_EXCL` on
POSIX). Existing files are not truncated because recovery is not implemented.

Physical headers are serialized field-by-field in canonical little-endian byte
order. Header CRCs are computed over those serialized bytes with the
corresponding header CRC field set to zero. The file header stores
`records_offset` and one application-owned `payload_schema_version`. The schema
version applies to every payload in the file and is not repeated in records.
Zero padding fills the gap between the canonical file header and the first
record so every record starts at an aligned offset.

## Test Boundary

The physical writer contains a narrow test control for failing a selected
record append or sync and counting calls. It is private to the component and is
not reachable through the public WAL API. Producer and consumer paths do not
consult it.
