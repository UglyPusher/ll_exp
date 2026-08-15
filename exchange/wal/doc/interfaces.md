# WAL Interfaces

## Idea In 15 Seconds

One object exposes three non-blocking roles over the same fixed-size payload
queue: enqueue, durability progression, and durable dequeue.

```cpp
class Wal {
public:
  OpenResult open(const std::filesystem::path& path,
                  const WalConfig& config) noexcept;

  EnqueueResult try_enqueue(std::span<const std::byte> payload) noexcept;

  DurabilityResult make_durable(
      std::uint32_t max_records = UINT32_MAX) noexcept;

  DequeueResult try_dequeue(std::span<std::byte> payload) noexcept;

  CloseResult close() noexcept;

  std::uint64_t accepted_sequence() const noexcept;
  std::uint64_t durable_sequence() const noexcept;
};
```

## Configuration

```cpp
struct WalConfig {
  std::uint32_t payload_size;
  std::uint32_t capacity;
  std::uint32_t alignment;
};
```

`payload_size` and `capacity` must be non-zero. `alignment` must be a power of
two and at least `alignof(void*)`. `open()` allocates the entire pool and creates
a new truncated WAL file. Opening an existing file is not implemented.

## Producer

`try_enqueue()` performs one bounded memory copy and publishes the next physical
sequence. It returns:

- `Ok` with the assigned sequence;
- `Full` when no pool slot can be reused;
- `InvalidPayloadSize` for a span of the wrong size;
- `IoError` after the durability path has failed;
- `Closed` when the WAL is not open.

## Durability Actor

`make_durable(limit)` persists at most `limit` records. One successful call uses
one stream flush for the selected range. Its result reports the resulting
durable frontier and the number of newly durable records.

This method is where a stronger durability backend can later be substituted.
It is not a producer append operation and it does not expose payload semantics.

## Consumer

`try_dequeue()` copies the oldest durable payload and releases its pool slot in
the same synchronous call. It returns:

- `Ok` with the physical sequence;
- `Empty` when the durable range contains no available payload;
- `InvalidPayloadSize` for a span of the wrong size;
- `Closed` when the WAL is not open.

The current copy-out API deliberately has no long-lived read lease. Therefore a
successful return is also the exact point at which the internal slot may be
reused. Durable consumer acknowledgements and replay checkpoints belong to a
future, separate contract.

## Shutdown

`close()` returns `PendingAccepted` and leaves the instance open if accepted
records still need durability processing. Otherwise it closes the physical file,
releases the pool, and returns either `Ok` or `IoError` from file close.
