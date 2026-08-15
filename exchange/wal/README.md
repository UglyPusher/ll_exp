# WAL

A bounded FIFO with one additional cursor: `durable`.

```text
read <= durable <= write
write - read <= capacity
```

The producer writes at `write`. The reader reads at `read`, but sees only the
range ending at `durable`, not the complete range ending at `write`. The WAL
durability mechanism persists `[durable, write)` and advances `durable`.

That is the complete queue model. The file format, CRC, batching limit, and
stream flush are implementation details of advancing the durable cursor.

## Current API

- `try_push(payload)` copies one fixed-size opaque payload and advances `write`.
- `advance_durable(limit)` persists up to `limit` queued payloads and advances
  `durable` after a successful stream flush.
- `try_pop(payload)` copies the next payload when `read < durable` and advances
  `read`.

All three cursors are absolute positions starting at zero. A physical record at
position `p` has sequence `p + 1`.

## Layout

- `include/fexma/wal/` - FIFO API, result types, and physical format.
- `src/` - ring storage, cursors, CRC, and file persistence.
- `tests/` - executable contract tests.
- `doc/` - contract, invariants, and interface details.

## Current Limits

- one producer, one durability mechanism caller, and one reader;
- fixed payload size and bounded capacity per WAL instance;
- aligned ring storage is allocated once in `open()`;
- `try_push()` and `try_pop()` copy payload bytes;
- create-and-truncate only; startup recovery is not implemented;
- stream `flush()` is the current durability policy; no `fsync` guarantee.
