# WAL

A bounded `RecordTape` with separately composable physical WAL persistence.

`RecordTape` owns only:

```text
tail <= retained positions < head
```

`RecordTape` provides producer publication, absolute-position immutable views,
bounded reclamation, and sequence exhaustion handling. `PersistenceModule`
owns live append/sync and its failure state. Their lifecycles are independent.
`PersistenceSlider` binds that module to `head`, owns its current position, and
publishes the durable progress frontier only after a complete batch sync.

`Slider` provides generic synchronous stage mechanics over the same retained
positions. It reads an upstream progress capability, invokes one statically
bound module, and is the sole user of its own progress writer capability.
Acquire and publish policies select the bounded range and publication cadence;
the slider owns no worker, polling loop, wait strategy, or domain semantics.
`NoOpModule` is the trivial successful stage used to prove the first bare
composition: `head -> NoOpSlider -> tail`.

The existing `Wal` class remains as a compatibility facade over the static
`RecordTape`/persistence-slider composition:

```text
producer -> [durable, head) -> physical sync -> [tail, durable) -> consumer
```

That compatibility composition exposes three absolute frontiers:

```text
tail <= durable <= head
head - tail <= capacity
```

`try_publish()` writes one block and publishes `head`. `advance_durable()` sets
the bounded acquire policy and invokes `PersistenceSlider`, which appends a
batch, performs one OS-level physical sync, and then publishes `durable`.
`try_consume()` exposes only positions below `durable` and publishes `tail`
after copying the block.

`open()` creates only a new WAL file and never truncates an existing path. The
physical file uses canonical little-endian headers and aligned record offsets.
Its immutable identity binds one stream kind and ID to one epoch, manifest, and
payload schema; runtime ring capacity is intentionally not persisted.

`WalReader` opens an existing file only when its complete persisted identity
matches the caller's expected configuration. It exposes a record only after
validating physical headers, sequence, CRCs, payload, and zero padding.
`scan_wal()` reports the longest trusted prefix without modifying the file.

`recover_incomplete_tail()` is the separate, conservative mutation boundary.
With exclusive ownership of a quiescent file, it may truncate only a physically
incomplete trailing record to the scanner-proven trusted offset, synchronize
that truncation, and validate the complete file again. Complete invalid records,
middle corruption, sequence errors, and identity mismatches are refused without
mutation.

After crash, the maximal contiguous CRC-valid prefix is the authoritative WAL:
every complete record in it participates in replay and rebuild regardless of
client acknowledgement. Batches are live append-and-sync units only. The file
contains no batch commit records or commit markers.

Start with [CONTRACT.md](doc/CONTRACT.md), then see
[DESIGN.md](doc/DESIGN.md) and [INVARIANTS.md](doc/INVARIANTS.md).
The physical layout is in [FILE_FORMAT.md](doc/FILE_FORMAT.md); build and test
commands are in [BUILDING.md](doc/BUILDING.md).
