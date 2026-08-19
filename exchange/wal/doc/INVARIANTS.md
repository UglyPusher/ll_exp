# WAL Invariants

## Frontier Order

The three frontiers are monotonic absolute zero-based positions:

```text
tail <= durable <= head
head - tail <= capacity
```

- `[tail, durable)` is readable.
- `[durable, head)` is published by the producer but not readable.
- `[head, tail + capacity)` is free capacity.
- Position `p` maps to block `p % capacity`.
- Position `p` maps to physical sequence `p + 1`.

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

## Publication Order

```text
producer --head--> durability writer --durable--> consumer --tail--> producer
```

- Payload copy happens before `head.store(..., release)`.
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
- No allocation occurs in publish, durability advance, or consume.

## Failure

- Empty durability batches do not append or synchronize.
- A failed append or sync does not move `durable`.
- No position at or above `durable` is returned by the consumer.
- I/O failure stops producer and durability progress, but not reading below the
  existing durable frontier.
- `close()` never releases storage while `tail != durable`.
- After an I/O failure, `close()` returns `PendingConsumption` until durable
  backlog is consumed, then releases resources and returns `IoError`.
