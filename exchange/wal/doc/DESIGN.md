# WAL Design

## Components

`Wal` owns lifecycle, storage, frontiers, failure state, and the selected
physical WAL adapter. It exposes lifecycle, the three role operations,
a diagnostic snapshot, and borrowed read-only access by absolute position.

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

`try_view(position)` validates the absolute position against `tail` and `head`
before mapping it with `position % capacity`. It returns position, derived
physical sequence, and a const span over the existing payload block. There is
no second data store or per-reader payload copy. Unlike `try_consume()`, it may
expose retained records that are not durable yet and never reclaims them.

The caller owns retention coordination: no reclaimer may pass a borrowed
position during access or use. The view is not a reader registration or a slot
pin. Lifecycle operations require all views to be retired. A future slider
must enforce its upstream permission separately from this storage-access check.
The existing producer, durability, consume, and close paths remain unchanged.

`PhysicalWalAdapter` owns the hardware-specific persistence mechanics. The
default filesystem implementation owns the native OS file handle, creates the
file, writes the file header, serializes physical records with CRC and padding,
synchronizes one completed batch, and closes the handle. It has no knowledge of
ring frontiers.

`physical_wal_adapter.hpp` is the single compile-time selection point. A
filesystem or direct-NVMe version is selected by including its concrete header
and building its corresponding source. `Wal` uses direct non-virtual calls;
there is no CRTP, runtime registry, or runtime backend selection.

`WalReader` is the cold-path validated sequential reader. Its selected
`PhysicalWalReaderAdapter` performs only hardware-specific byte reads;
`WalReader` owns canonical decoding, identity checks, CRC validation, sequence
validation, and fail-closed state. `scan_wal()` drives the same reader to report
the longest trusted prefix and never mutates storage.

`recover_incomplete_tail()` is a separate cold-path policy layer. It uses
`scan_wal()` as the sole source of the trusted truncation offset and calls the
compile-time selected `PhysicalWalRecoveryAdapter` only for a scanner-proven
incomplete trailing record. The adapter performs the hardware-specific
truncate-and-sync operation. Recovery then scans the complete retained file
again; it never repairs, skips, or resynchronizes around corruption.

## Operation Walkthrough

`try_publish()` validates the call, uses the block at `head`, fills it,
publishes `head + 1`, and returns the derived physical sequence.

`advance_durable()` selects a bounded pending range, passes each immutable block
to the selected `PhysicalWalAdapter`, requests one physical sync, and publishes
the range end as `durable` only after success.

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
POSIX). The live writer never truncates an existing file. Explicit recovery
opens the quiescent file separately and truncates only to a scanner-proven
record boundary.

Filesystem recovery uses `SetEndOfFile` followed by `FlushFileBuffers` on
Windows, `ftruncate` followed by `fdatasync` on POSIX, and `ftruncate` followed
by `fsync` on macOS. The caller provides exclusive file ownership across the
initial scan, mutation, and verification scan, preventing a concurrent writer
from invalidating the trusted offset.

Physical headers are serialized field-by-field in canonical little-endian byte
order. Header CRCs are computed over those serialized bytes with the
corresponding header CRC field set to zero. The immutable file header identifies
one stream kind and ID, one epoch, one manifest, one first sequence, and one
application-owned payload schema. Runtime ring capacity is not persisted.
Stream, epoch, manifest, and schema metadata apply to every payload in the file
and are not repeated in records. Zero padding fills the gap between the
canonical file header and the first record so every record starts at an aligned
offset.

## Test Boundary

The physical writer contains a narrow test control for failing a selected
record append or sync and counting calls. It is private to the component and is
not reachable through the public WAL API. Producer and consumer paths do not
consult it. The filesystem recovery adapter has an equivalent private control
for truncate and sync failure-path tests.
