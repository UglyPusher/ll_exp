# WAL Contract

## Idea In 15 Seconds

The WAL is a bounded queue whose visibility is controlled by durability. A
producer copies opaque fixed-size bytes into memory. A durability actor persists
them in order. A consumer receives only records already covered by the durable
frontier.

The file is a persistence mechanism behind the queue, not the consumer API.

## Terminology

- `payload` - opaque bytes supplied by the caller, always `payload_size` long.
- `pool slot` - one aligned, reusable in-memory storage element.
- `record` - physical record header plus one payload in the WAL file.
- `WAL file` - file header followed by physical records.
- `accepted_sequence` - highest payload copied into the in-memory queue.
- `durable_sequence` - highest contiguous accepted sequence completed by the
  current durability policy.

There is no public `consumed_sequence`. Successful dequeue returns that one pool
slot for reuse. It is not a durable consumer checkpoint and carries no domain
meaning.

## Ownership And Lifetime

The WAL allocates and owns `capacity` aligned pool slots during `open()`. It
releases them during `close()` or destruction. No queue operation allocates.

The caller owns spans passed to `try_enqueue()` and `try_dequeue()`.

- `try_enqueue()` copies from the input span before returning.
- `try_dequeue()` copies into the output span before returning.
- The WAL does not retain either span or access caller memory after return.

This is a synchronous span contract. No async span lifetime is defined.

## Queue State

Each sequence passes through these states in order:

```text
free slot -> accepted -> durable -> dequeued/released -> free slot
```

`try_enqueue()` returns `Ok` after the payload copy is complete and the accepted
frontier is published. The result contains the WAL-assigned physical sequence.
It makes no durability claim.

`try_enqueue()` returns `Full` when all pool slots contain accepted records that
have not yet been dequeued. Backpressure is immediate; the call does not wait.

`try_dequeue()` returns `Empty` when no durable, not-yet-dequeued payload exists.
Accepted records above the durable frontier remain invisible.

## Durability

`make_durable(max_records)` selects up to `max_records` contiguous accepted
records immediately after `durable_sequence`. It writes their physical records
in sequence order and calls `std::ostream::flush()` once for the selected range.

On success it atomically advances `durable_sequence` to the end of that range.
A return with `records == 0` is successful and leaves the frontier unchanged.

On write or flush failure it returns `IoError`, does not advance the durable
frontier, and places the instance in an I/O-failed state. Further enqueue and
durability attempts report `IoError`. Recovery from a partial physical write is
not implemented.

The current durability policy is exactly C++ stream flush. The implementation
does not call `fsync`, `FlushFileBuffers`, or an equivalent OS API, so
power-loss and kernel-crash durability are open contracts.

## Roles And Concurrency

The current contract permits exactly one caller for each role:

- producer calls `try_enqueue()`;
- durability actor calls `make_durable()`;
- consumer calls `try_dequeue()`.

The three roles may execute on different threads. Publication between roles uses
release/acquire frontiers. `open()` and `close()` require external quiescence and
must not overlap queue operations. Multiple producers, durability actors, or
consumers are outside the contract.

## Close

`close()` returns `PendingAccepted` while accepted records remain above the
durable frontier and leaves the WAL open. Durable records do not have to be
dequeued before close.

The destructor closes the file and releases memory but does not make pending
records durable.
