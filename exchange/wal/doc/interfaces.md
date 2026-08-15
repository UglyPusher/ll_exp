# WAL Interfaces

## Idea In 15 Seconds

The API is a bounded FIFO with push, pop, and one operation that advances the
durable cursor.

```cpp
class Wal {
public:
  OpenResult open(const std::filesystem::path& path,
                  const WalConfig& config) noexcept;

  PushResult try_push(std::span<const std::byte> payload) noexcept;

  DurabilityResult advance_durable(
      std::uint32_t max_records = UINT32_MAX) noexcept;

  PopResult try_pop(std::span<std::byte> payload) noexcept;

  CloseResult close() noexcept;

  std::uint64_t read_cursor() const noexcept;
  std::uint64_t durable_cursor() const noexcept;
  std::uint64_t write_cursor() const noexcept;
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
two and at least `alignof(void*)`. `open()` allocates the complete ring and
creates a new truncated WAL file.

## Push

`try_push()` returns:

- `Ok` with the physical sequence after copying and advancing `write`;
- `Full` when `write - read == capacity`;
- `InvalidPayloadSize` for a span of the wrong size;
- `IoError` after persistence has failed;
- `Closed` when the WAL is not open.

## Advance Durable

`advance_durable(limit)` processes at most `limit` positions from
`[durable, write)`. Its result reports the resulting `durable_cursor` and the
number of positions advanced. An empty range is a successful no-op.

The current implementation writes physical records and performs one stream
flush per non-empty call. A stronger persistence mechanism can replace that
detail without changing the three-cursor queue model.

## Pop

`try_pop()` returns:

- `Ok` with the physical sequence after copying and advancing `read`;
- `Empty` when `read == durable`;
- `InvalidPayloadSize` for a span of the wrong size;
- `Closed` when the WAL is not open.

There is no read lease or acknowledgement API. The output copy and slot release
are one synchronous operation.

## Shutdown

`close()` returns `PendingDurability` if `durable != write`. Otherwise it closes
the physical file, releases the ring, and returns `Ok` or `IoError`.
