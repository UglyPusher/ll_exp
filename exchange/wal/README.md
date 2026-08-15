# WAL

A bounded in-memory queue with an explicit durability frontier.

The producer places fixed-size opaque payloads into a preallocated pool. The
durability actor persists accepted payloads in physical sequence order. The
consumer can dequeue only payloads at or below the durable frontier.

```text
producer -> accepted in memory -> durable -> consumer
               |                    ^
               +---- WAL file ------+
```

The WAL knows payload bytes, physical sequence, CRC, and durability state. It
does not know commands, events, matcher types, consumers, or domain sequence.

## Current API

- `try_enqueue(payload)` copies one payload into a free pool slot and advances
  `accepted_sequence`.
- `make_durable(limit)` writes accepted records in order, flushes the stream,
  and advances `durable_sequence` only after the whole selected range succeeds.
- `try_dequeue(payload)` copies the oldest durable payload out and immediately
  returns its slot to the pool.

`try_enqueue()` success does not mean durable. `make_durable()` is the only
operation that advances durability.

## Layout

- `include/fexma/wal/` - queue API, statuses, and physical format.
- `src/` - pool, queue frontiers, CRC, and file persistence.
- `tests/` - executable contract tests.
- `doc/` - contract, invariants, and interface details.

## Current Limits

- one producer, one durability actor, and one consumer;
- fixed payload size and bounded capacity per WAL instance;
- pool allocation happens once in `open()`; queue operations do not allocate;
- create-and-truncate only; startup recovery is not implemented;
- stream `flush()` is the current durability policy; no `fsync` guarantee;
- no batching policy, background thread, segment rotation, or async API.
