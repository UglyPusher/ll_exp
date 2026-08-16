# WAL Contract

## Public API

```cpp
class Wal {
public:
  OpenResult open(const std::filesystem::path& path,
                  const WalConfig& config) noexcept;

  PublishResult try_publish(std::span<const std::byte> payload) noexcept;
  DurabilityResult advance_durable(std::uint32_t batch_size) noexcept;
  ConsumeResult try_consume(std::span<std::byte> payload) noexcept;

  CloseResult close() noexcept;

  bool is_open() const noexcept;
  const WalConfig& config() const noexcept;
  WalSnapshot snapshot() const noexcept;
};
```

`snapshot()` is diagnostic. Its frontiers are not mutable controls.

## Roles

One producer calls `try_publish()`, one durability writer calls
`advance_durable()`, and one consumer calls `try_consume()`. The three roles may run
concurrently on separate threads. A second caller for any role is outside the
contract.

`open()` and `close()` require all role threads to be stopped.

## Payload And Lifetime

Each WAL instance has one non-zero `payload_size` and bounded non-zero
`capacity`. Payload bytes are opaque to the WAL.

Input and output spans remain owned by the caller. Each operation finishes its
copy synchronously and never retains the span or accesses caller memory after
return.

## Publish

`try_publish()` is non-blocking and allocation-free. On success it copies one
payload into the block at `head`, publishes `head + 1`, and returns physical
sequence `position + 1`.

It returns `Full` when `head - tail == capacity`. Success does not mean the
payload is durable or visible to the consumer.

After a durability I/O failure, producer calls return `IoError` without
publishing more data.

## Advance Durable

`advance_durable(batch_size)` selects at most `batch_size` positions from
`[durable, head)`. An empty selection is a successful no-op and performs no
physical sync.

For a non-empty batch the physical writer:

1. appends every physical record in sequence order;
2. performs exactly one OS-level physical synchronization;
3. reports success to the ring.

Only then does the ring publish the batch end as the new `durable` frontier.

Physical synchronization is `FlushFileBuffers` on Windows, `fdatasync` on
POSIX, and `fsync` on macOS. Creating a file also synchronizes its initial
header; POSIX creation additionally synchronizes the parent directory entry.

Append or sync failure leaves `durable` unchanged and permanently puts producer
and durability operations into fail-closed `IoError` state.

## Consume

`try_consume()` is non-blocking and allocation-free. It returns `Empty` when
`tail == durable`, including when non-durable data exists above that frontier.

On success it copies the next durable payload, advances `tail`, and returns its
physical sequence. Advancing `tail` makes the block reusable; it is not a
durable consumer acknowledgement or domain checkpoint.

Consumer operation does not inspect the physical writer or I/O failure state. It
can drain the previously published durable range after a later I/O failure.

## Lifecycle

`open()` validates configuration, allocates and warms all ring storage, creates
and physically synchronizes a new truncated WAL file, resets frontiers, and only
then publishes the working state.

`close()` returns `PendingDurability` without closing while a healthy WAL has
`durable != head`. After an I/O failure, `close()` releases resources and
returns `IoError`, because the non-durable tail can no longer be committed by
that instance.

Destruction releases resources but never advances durability.

## Intentional Limits

- Existing WAL files cannot be opened or recovered.
- Partial-tail validation and truncation are not implemented.
- There is no segment rotation, compaction, or consumer checkpoint.
- Storage is one monolithic allocation, not an external block pool.
- The model is a three-stage SPSC frontier chain, not a broadcast SPMC tract.
- NUMA placement and CRC acceleration are not implemented.
