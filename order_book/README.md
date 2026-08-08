# fexma::order_book

`fexma::order_book` is fixed-capacity in-memory storage for resting Bid and Ask
orders.

It stores orders, maintains price priority and FIFO order at each price, returns
the best resting order on either side, updates remaining quantity, and erases by
`OrderId`. It is not a matching engine: matching policy, trades, accounts, WAL,
networking, callbacks, and synchronization are deliberately outside the
component.

Calls must be externally serialized by a single writer. The implementation has
no locks or atomics.

## Requirements

- C++20 compiler and standard library
- CMake 3.16 or newer
- 64-bit `std::size_t`; Windows x64 with MSVC 19.44 is verified

See [PORTABILITY.md](PORTABILITY.md) for the verified and pending platform
matrix.

## Quick Start

From the repository root on Windows:

```powershell
cmake --preset windows-msvc
cmake --build --preset windows-msvc-release --target order_book_basic_usage
..\build\windows-msvc\order_book\Release\order_book_basic_usage.exe
```

For a single-configuration CMake generator:

```sh
cmake -S . -B ../build/order-book -DCMAKE_BUILD_TYPE=Release
cmake --build ../build/order-book --target order_book_basic_usage
../build/order-book/order_book/order_book_basic_usage
```

Minimal usage:

```cpp
#include <fexma/order_book/order_book.hpp>

using namespace fexma::order_book;

OrderBook book({100, 200, 16});

const InsertResult inserted =
    book.insert({1, 42, Side::Bid, 150, 10});
const InsertResult ask_inserted =
    book.insert({2, 43, Side::Ask, 155, 12});
if (!inserted.ok() || !ask_inserted.ok()) {
  return 1;
}

const auto best_bid = book.best(Side::Bid);
const auto best_ask = book.best(Side::Ask);
if (!best_bid || best_bid->id != 1 || !best_ask || best_ask->id != 2) {
  return 2;
}

const SetRemainingResult changed = book.set_remaining(1, 7);
if (!changed.ok() || changed.previous_remaining != 10) {
  return 3;
}

const EraseResult erased = book.erase(2);
if (!erased.ok() || erased.removed.id != 2 ||
    !book.validate_invariants()) {
  return 4;
}
```

The complete compilable example is
[examples/basic_usage.cpp](examples/basic_usage.cpp).

## Public API

| Type or operation | Contract |
|---|---|
| `OrderBookConfig` | Inclusive price range and fixed order capacity. |
| `RestingOrderData` | Value used to insert one resting order. |
| `insert(order)` | Append to the side/price FIFO; return an explicit status. |
| `best(side)` | Return a value snapshot of the best order, or `std::nullopt`. |
| `set_remaining(id, quantity)` | Update quantity without changing FIFO position. |
| `erase(id)` | Remove by ID and return a snapshot of the removed order. |
| `validate_invariants()` | Perform a slow, complete structural consistency check. |

`Quantity` is `uint32_t` for one order. `AggregateQuantity` is `uint64_t`, so
level and side totals do not wrap at `UINT32_MAX`.

## Runtime Guarantees

- Construction allocates and sequentially first-touches all fixed storage.
- `insert`, `best`, `set_remaining`, and `erase` do not allocate after
  construction.
- Equal-price orders are returned in FIFO order.
- Bid selects the maximum price; Ask selects the minimum price.
- `set_remaining` preserves FIFO position; quantity zero is rejected and
  removal is performed through `erase`.
- Failed operations leave the logical book unchanged.
- Construction can throw `std::bad_alloc`; scalar type limits are not promises
  that a configuration fits in memory.
- `validate_invariants()` is a debug/test operation, may allocate, and is not a
  hot-path API.

## Test Tiers

Ordinary CTest runs component tests, the 6K-operation randomized test, and the
600K-operation quick soak. The 300K randomized stress, 6M soak stress, and 100M
soak are explicit commands and are not registered as ordinary CTest tests.

See [tests/README.md](tests/README.md) for exact commands and reproduction
options.

## Further Reading

- [DESIGN.md](DESIGN.md) - structures, invariants, complexity, and memory model
- [BENCHMARKS.md](BENCHMARKS.md) - benchmark targets and measurement rules
- [PORTABILITY.md](PORTABILITY.md) - verified toolchains and platform limits
