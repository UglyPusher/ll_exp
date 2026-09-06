# WAL Invariants

## Frontier Order

The three frontiers are monotonic absolute zero-based positions:

```text
tail <= durable <= head
head - tail <= capacity
```

- `[tail, durable)` is readable by `try_consume()`.
- `[durable, head)` is published but not readable by `try_consume()`.
- `[tail, head)` is accessible through read-only `try_view()` under the
  caller-owned retention contract; availability does not prove durability.
- `[head, tail + capacity)` is free capacity.
- Position `p` maps to block `p % capacity`.
- Position `p` maps to physical sequence `first_sequence + p` without unsigned
  wraparound.

Only the producer writes `head`, only the durability writer writes `durable`,
and only the consumer writes `tail`.

## Ownership

- A free block is owned by the producer while it fills the payload.
- Publication of `head` transfers the immutable block to the pending range.
- The durability writer borrows pending blocks without modifying them.
- Publication of `durable` makes the block available to the consumer.
- The consumer owns the block while copying its payload.
- Publication of `tail` releases the block for producer reuse.

A block cannot be overwritten until `tail` passes its previous absolute
position.

Borrowed view readers do not own or advance a frontier. Before obtaining a view
and throughout its use, they must ensure reclamation cannot pass its position.
The view neither pins the slot nor survives close/destruction/reopen. Absolute
range validation precedes slot mapping, preventing a reclaimed position from
being interpreted as the new record in a reused slot under this contract.

## Publication Order

```text
producer --head--> durability writer --durable--> consumer --tail--> producer
```

- Payload copy happens before `head.store(..., release)`.
- Retained view access acquires `head` before exposing payload bytes.
- Durability acquires `head` before reading pending blocks.
- Record append and physical sync happen before
  `durable.store(..., release)`.
- Consumer acquires `durable` before reading a block.
- Consumer copy happens before `tail.store(..., release)`.
- Producer acquires `tail` before reusing capacity.

No frontier operation uses `seq_cst`.

## Memory

- Storage contains exactly `capacity` fixed-stride blocks.
- Every block address satisfies configured `alignment`.
- The complete allocation is zeroed during `open()` to commit and touch every
  page before role threads start.
- No allocation occurs in publish, durability advance, consume, or position view.

## Failure

- Empty durability batches do not append or synchronize.
- A failed append or sync does not move `durable`.
- No position at or above `durable` is returned by `try_consume()`; retained
  views remain independent of durability and do not grant downstream permission.
- I/O failure stops producer and durability progress, but not reading below the
  existing durable frontier.
- `close()` never releases storage while `tail != durable`.
- After an I/O failure, `close()` returns `PendingConsumption` until durable
  backlog is consumed, then releases resources and returns `IoError`.

## Post-Crash Tail

- The maximal contiguous CRC-valid prefix is the authoritative recovered WAL.
- Every complete record in that prefix participates in replay and rebuild.
- Client acknowledgement state does not change the recovered tail.
- Batches and the runtime `durable` frontier are not persisted as separate
  commit metadata.
- The physical format has no batch commit record or commit marker.
