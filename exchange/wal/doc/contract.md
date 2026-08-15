# WAL Contract

## Idea In 15 Seconds

The WAL is an ordinary bounded FIFO with `read` and `write` cursors plus a
`durable` cursor between them.

```text
read <= durable <= write
```

The reader treats `durable`, not `write`, as the end of the readable queue. The
durability mechanism persists `[durable, write)` and moves `durable` forward.

## Cursors

All cursors are absolute zero-based positions and are exclusive boundaries:

- `read` is the position of the next payload to pop;
- `durable` is one past the last payload available to the reader;
- `write` is the position where the producer will push the next payload.

The queue contains `[read, write)`. Its readable part is `[read, durable)`. Its
not-yet-durable part is `[durable, write)`.

Physical WAL sequence is derived from position and starts at one:

```text
sequence = position + 1
```

It has no domain meaning.

## Payload And Ownership

A payload is exactly `payload_size` opaque bytes. The WAL does not know command,
event, matcher, schema, consumer, or domain sequence semantics.

The WAL owns `capacity` aligned ring slots allocated during `open()`. Queue
operations do not allocate.

The caller owns spans passed to `try_push()` and `try_pop()`:

- `try_push()` finishes copying from the span before it advances `write`;
- `try_pop()` finishes copying into the span before it advances `read`;
- the WAL does not retain either span after the synchronous call returns.

## Push

`try_push()` writes at `write % capacity`. It returns `Full` when
`write - read == capacity`; it never waits for space.

Success advances `write` and returns the physical sequence. Success means only
that the payload is in the in-memory queue. It makes no durability claim.

## Durability

`advance_durable(max_records)` selects up to `max_records` positions from
`[durable, write)`, writes their physical records in order, and calls
`std::ostream::flush()` once.

Only after all selected writes and the flush succeed does it publish the new
`durable` cursor. On failure it returns `IoError`, leaves `durable` unchanged,
and makes later push and durability calls fail closed.

The current durability policy is exactly C++ stream flush. The implementation
does not call `fsync`, `FlushFileBuffers`, or an equivalent OS API. Stronger
crash durability remains an open contract.

## Pop

`try_pop()` returns `Empty` when `read == durable`, even when `durable < write`.
Otherwise it copies the payload at `read % capacity`, advances `read`, and
returns the physical sequence.

Advancing `read` permits the producer to reuse that ring slot. It is local queue
mechanics, not a durable acknowledgement or domain consumer checkpoint.

## Concurrency

The current contract permits one caller per moving cursor:

- one producer advances `write`;
- one durability mechanism caller advances `durable`;
- one reader advances `read`.

These roles may run on separate threads. Cursor publication uses release/acquire
ordering. `open()` and `close()` require all queue activity to be stopped.

## Close

`close()` returns `PendingDurability` while `durable != write` and leaves the WAL
open. Already durable payloads do not have to be popped before close.

Destruction releases resources but does not advance durability.
