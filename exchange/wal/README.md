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

Start with [CONTRACT.md](doc/CONTRACT.md), then see
[DESIGN.md](doc/DESIGN.md) and [INVARIANTS.md](doc/INVARIANTS.md).
The physical layout is in [FILE_FORMAT.md](doc/FILE_FORMAT.md); build and test
commands are in [BUILDING.md](doc/BUILDING.md).
