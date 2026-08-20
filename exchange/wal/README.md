# WAL

A bounded WAL frontier ring with an intermediate physical durability stage:

```text
producer -> [durable, head) -> physical sync -> [tail, durable) -> consumer
```

The ring owns fixed-size opaque payload blocks and three absolute frontiers:

```text
tail <= durable <= head
head - tail <= capacity
```

`try_publish()` writes one block and publishes `head`. `advance_durable()`
appends a batch to the WAL file, performs one OS-level physical sync, and then
publishes `durable`. `try_consume()` exposes only positions below `durable` and
publishes `tail` after copying the block.

`open()` creates only a new WAL file and never truncates an existing path. The
physical file uses canonical little-endian headers and aligned record offsets.
Its immutable identity binds one stream kind and ID to one epoch, manifest, and
payload schema; runtime ring capacity is intentionally not persisted.

`WalReader` opens an existing file only when its complete persisted identity
matches the caller's expected configuration. It exposes a record only after
validating physical headers, sequence, CRCs, payload, and zero padding.
`scan_wal()` reports the longest trusted prefix without modifying the file.

Start with [CONTRACT.md](doc/CONTRACT.md), then see
[DESIGN.md](doc/DESIGN.md) and [INVARIANTS.md](doc/INVARIANTS.md).
The physical layout is in [FILE_FORMAT.md](doc/FILE_FORMAT.md); build and test
commands are in [BUILDING.md](doc/BUILDING.md).
