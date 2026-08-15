# WAL Invariants

## Idea In 15 Seconds

The WAL is a bounded ring governed by three monotonically increasing cursors:

```text
read <= durable <= write
write - read <= capacity
```

## Cursor Invariants

- Cursors start at zero and move forward only.
- Only the producer writes `write`.
- Only the durability mechanism writes `durable`.
- Only the reader writes `read`.
- `[read, durable)` is readable.
- `[durable, write)` is queued but not readable.
- A position `p` maps to slot `p % capacity`.
- A position `p` maps to physical sequence `p + 1`.
- The producer reuses a slot only after `read` has passed its previous position.

## Publication Invariants

- Producer payload copy happens before publishing the new `write`.
- Durability reads only positions below an acquired `write`.
- Physical persistence and stream flush happen before publishing `durable`.
- Reader reads only positions below an acquired `durable`.
- Reader payload copy happens before publishing the new `read`.

The ordering chain is:

```text
producer --write--> durability --durable--> reader --read--> producer
```

## Memory Invariants

- The ring contains exactly `capacity` slots.
- Every slot begins at an address aligned to `alignment`.
- Slot stride is `payload_size` rounded up to `alignment`.
- Push and pop spans are exactly `payload_size` bytes.
- No queue operation allocates or retains caller spans.

## Physical File Invariants

- The file begins with one `FileHeader`.
- `FileHeader` stores format magic, version, compiled header size, payload size,
  record alignment, initial next sequence, header CRC, and reserved bytes.
- The create path writes `FileHeader::next_sequence == 1` and does not update it.
- Each record contains `RecordHeader`, one payload, and zero padding to the
  configured alignment.
- Record headers contain physical sequence, payload CRC32, and header CRC32.
- Records are written contiguously in physical sequence order from one.
- Capacity and cursor positions are not stored in the file format.

## Failure Invariants

- Failed write or flush does not advance `durable`.
- A position at or above `durable` is never returned by `try_pop()`.
- After an I/O failure, push and durability operations fail closed.
- `close()` does not silently discard `[durable, write)`; it returns
  `PendingDurability` and leaves the WAL open.

## Open Contracts

- OS-level crash durability beyond `std::ostream::flush()`.
- Recovery and reconstruction of cursors from an existing WAL file.
- Validation or truncation of a partial final physical record.
- Cursor overflow.
- Multiple writers of the same cursor.
