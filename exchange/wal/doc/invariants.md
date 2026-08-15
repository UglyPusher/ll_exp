# WAL Invariants

## Idea In 15 Seconds

The accepted frontier never moves past available pool capacity, the durable
frontier never moves past accepted data, and the consumer never sees data above
the durable frontier.

## Frontier Invariants

Let `released` be the internal sequence of the last slot returned to the pool.
It is implementation bookkeeping, not a public consumer checkpoint.

```text
released <= durable_sequence <= accepted_sequence
accepted_sequence - released <= capacity
```

- WAL physical sequences start at `1` and are contiguous.
- Only the producer advances `accepted_sequence`.
- Only the durability actor advances `durable_sequence`.
- Only successful dequeue advances internal `released` state.
- A slot is overwritten only after its previous sequence was released.
- A payload is dequeued at most once by the current single consumer.

## Memory Invariants

- The pool contains exactly `capacity` slots.
- Every slot starts at an address aligned to `alignment`.
- Slot stride is `payload_size` rounded up to `alignment`.
- Every enqueue and dequeue span is exactly `payload_size` bytes.
- Producer copy completes before the accepted frontier publishes the sequence.
- Durability reads only accepted slots.
- Consumer reads only durable slots.
- Consumer copy completes before the slot is released for producer reuse.

## Physical File Invariants

- The file begins with one `FileHeader`.
- `FileHeader` stores format magic, version, compiled header size, payload size,
  record alignment, initial next sequence, header CRC, and reserved bytes.
- The current create path writes `FileHeader::next_sequence == 1` and does not
  update the field later.
- Each physical record contains `RecordHeader`, one payload, and zero padding to
  the configured alignment.
- `RecordHeader` stores format magic, version, compiled header size, WAL physical
  sequence, payload CRC32, and header CRC32.
- Physical record sequences are written contiguously from `1`.
- Pool capacity and queue release state are not stored in the file format.

## Failure Invariants

- A failed write or flush does not advance `durable_sequence`.
- A payload above `durable_sequence` is never delivered.
- After an I/O failure, producer and durability operations fail closed.
- `close()` does not discard accepted, non-durable records during an ordinary
  successful call; it reports `PendingAccepted` and leaves the WAL open.

## Open Contracts

- OS-level crash durability beyond `std::ostream::flush()`.
- Recovery, file validation, and reconstruction of queued records at startup.
- Truncation of a partial final physical record.
- Sequence overflow.
- Behaviour outside the one-producer, one-durability-actor, one-consumer model.
