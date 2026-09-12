# WAL Design

## Components

`RecordTape` owns runtime lifecycle, bounded storage, `head`, `tail`, producer
publication, reclamation, position exhaustion, and borrowed read-only position
access. It has no filesystem path, physical writer, durable frontier, or I/O
failure state.

`PersistenceModule` owns the selected live physical WAL adapter and persistence
failure state. It accepts immutable `RecordView` values, appends them using the
unchanged physical format, and synchronizes when instructed. It does not own a
position, select a batch, or publish progress.

`PersistenceSlider` is the dedicated bounded persistence stage. It appends the
selected `RecordTape` range, performs one sync, and publishes its durable
frontier only after the complete batch succeeds.

`Slider` is a header-only template over only its concrete module. Its source is
`RecordTape`; it reads either the tape head or an explicit upstream `Frontier`,
processes the range visible in one upstream observation, and publishes its own
frontier after each successful record. The caller owns repeated execution and
all waiting or scheduling.

`Frontier` owns one monotonic exclusive-end atomic. Ordinary constness separates
capabilities: an upstream `const Frontier&` can only acquire, while Slider holds
its own `Frontier&` and may publish. This preserves one runtime publisher for
every intermediate frontier. Persistence uses its separate stage because its
frontier publication is conditional on batch sync.

The first concrete static composition uses `NoOpModule`:

```text
producer -> RecordTape::head -> Slider<NoOpModule> -> Frontier -> reclaimer -> tail
```

The slider publishes its `Frontier`; after the synchronous slider call retires
all borrowed views, the composition reads that frontier and advances `tail`.
These are distinct operations. If the composition does not run the slider or
does not reclaim, the bounded producer eventually observes `Full`.

`RecordTape::Storage` owns one aligned allocation. It implements exactly four lifecycle and
addressing responsibilities:

```text
initialize/allocate -> warm/touch -> block_at_slot(slot) -> release
```

It does not implement frontier policy or persistence.

`RecordTape` keeps absolute `tail` and `head` boundaries. The producer keeps a
cached head slot for its sequential hot path. Absolute view access derives the
slot only after validating the position. Reclamation publishes an exclusive
absolute boundary and needs no slot cursor.

`try_view(position)` validates the absolute position against `tail` and `head`
before mapping it with `position % capacity`. It returns position and a const
span over the existing payload block. There is
no second data store or per-reader payload copy. It may expose retained records
that are not durable yet and never reclaims them.

The caller or composition owns retention coordination: no reclaimer may pass a
borrowed position during access or use. The view is not a reader registration
or a slot pin. Lifecycle operations require all views to be retired. `Slider`
enforces its upstream permission separately from this storage-access check.

`PhysicalWalAdapter` owns the hardware-specific persistence mechanics. The
default filesystem implementation owns the native OS file handle, creates the
file, writes the file header, serializes physical records with CRC and padding,
synchronizes one completed batch, and closes the handle. It has no knowledge of
ring frontiers.

`physical_wal_adapter.hpp` is the single compile-time selection point. A
filesystem or direct-NVMe version is selected by including its concrete header
and building its corresponding source. `PersistenceModule` uses the selected
concrete type directly; there is no CRTP, runtime registry, virtual dispatch,
or runtime backend selection.

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

`RecordTape::try_publish()` validates the call, uses the block at `head`, fills it,
publishes `head + 1`, and returns the published position.

The composition sets the persistence slider's maximum batch size and invokes
`process_available()`. `PersistenceSlider` obtains each `RecordView`, calls
`PersistenceModule::process()`, requests one sync, and publishes the range end
as `durable` only after success. `PersistenceModule` derives physical WAL
sequence from its own `first_sequence` and the tape position.

## Frontier Layout

Each `RecordTape` boundary and slider `Frontier` is stored in its own
explicitly padded 64-byte aligned object.
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
