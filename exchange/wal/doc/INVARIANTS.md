# WAL Invariants

## RecordTape Frontier Order

`RecordTape` intrinsically owns only two monotonic absolute zero-based
boundaries:

```text
tail <= head
head - tail <= capacity
```

- `[tail, head)` contains published retained positions.
- `[head, tail + capacity)` is free capacity.
- Only the producer writes `head`.
- Only the composition/reclaimer writes `tail`.
- `reclaim(end)` cannot move backward or beyond observed `head`.
- The composition advances `tail` only after every mandatory reader has
  finished the reclaimed positions.

`RecordTape` contains no `durable` frontier, file writer, or persistence failure
state.

## Slider Frontier

Every slider frontier and current value is an exclusive end:

```text
own frontier <= slider current <= observed upstream frontier
```

- The upstream reference is const and therefore read-only.
- Only the slider holding its own non-const frontier reference publishes it.
- A range must begin at `current` and end no later than the acquired upstream
  frontier.
- A record view is obtained by its absolute zero-based position.
- The module must complete position `p` successfully before current becomes
  `p + 1`.
- Own frontier publication occurs only after successful module completion.
- Release publication of exclusive end `p + 1` makes module output for
  position `p` visible to an acquire-reading downstream stage.
- The composition may reclaim only through the last mandatory published
  frontier and only after all borrowed views have been retired.

The slider mechanics allocate no storage, copy no payload, interpret no
record kind, and perform no persistence, snapshot I/O, waiting, or scheduling.

For the bare linear composition:

```text
tail <= NoOpF <= head
```

Publication of `NoOpF` certifies module completion but does not reclaim a
position. Only the composition writes `tail`, after the corresponding slider
call has returned and its borrowed views are retired. Stopping either slider
execution or composition reclamation therefore preserves bounded
backpressure.

## Persistence Composition Frontier Order

A composition may maintain three frontiers while `durable` is owned by
`PersistenceSlider`:

```text
tail <= durable <= head
head - tail <= capacity
```

- `[tail, durable)` is available to a downstream stage.
- `[durable, head)` is published but not yet durable.
- `[tail, head)` is accessible through read-only `try_view()` under the
  caller-owned retention contract; availability does not prove durability.
- `[head, tail + capacity)` is free capacity.
- Position `p` maps to block `p % capacity`.
- Position `p` maps to physical sequence `first_sequence + p` without unsigned
  wraparound.

Only the producer writes `head`, only `PersistenceSlider` writes `durable`, and
only the composition reclaimer writes `tail`.

## Ownership

- A free block is owned by the producer while it fills the payload.
- Publication of `head` transfers the immutable block to the pending range.
- `PersistenceModule` borrows pending blocks without modifying them and appends
  their immutable contents to physical storage.
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
  the persistence slider's `durable.store(..., release)`.
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
- Retained views remain independent of durability and do not grant downstream
  permission.
- A failed persistence module prevents further durable progress.

`PersistenceModule` owns its terminal failure state; `RecordTape` remains
unaware of it. A composition with mandatory persistence must stop its producer
after observing that failure.

## Post-Crash Tail

- The maximal contiguous CRC-valid prefix is the authoritative recovered WAL.
- Every complete record in that prefix participates in replay and rebuild.
- Client acknowledgement state does not change the recovered tail.
- Batches and the runtime `durable` frontier are not persisted as separate
  commit metadata.
- The physical format has no batch commit record or commit marker.
