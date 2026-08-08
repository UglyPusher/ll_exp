# fexma::order_book

Standalone single-threaded storage for resting orders in a low-latency matching
engine. The library owns bid/ask books, intrusive FIFO price levels, a fixed
order pool, and a fixed-capacity `OrderId -> OrderIndex` index.

It deliberately does not implement matcher policy, STP, events, WAL, accounts,
networking, callbacks, virtual interfaces, locks, atomics, or session state.

## Public API

The runtime contract is intentionally small:

- `InsertResult insert(const RestingOrderData& order) noexcept`
- `std::optional<OrderView> best(Side side) const noexcept`
- `SetRemainingResult set_remaining(OrderId id, Quantity new_remaining) noexcept`
- `EraseResult erase(OrderId id) noexcept`
- `bool validate_invariants() const noexcept`

Construction allocates and initializes all fixed storage:

- `OrderBook(const OrderBookConfig& config)`

The book is ready for runtime operations immediately after construction.

## Operation Semantics

`insert` appends one resting order to the tail of its side/price FIFO. Rejected
orders leave the logical book unchanged. Public failure statuses are duplicate
ID, capacity exhausted, price out of range, and invalid quantity.

`best(Side::Bid)` returns the Bid order with the maximum price.
`best(Side::Ask)` returns the Ask order with the minimum price. Equal-price
orders are returned in FIFO order. The result is a value snapshot and does not
create hidden selected state.

`set_remaining` updates quantity by `OrderId` without changing side, price, or
FIFO position. `new_remaining == 0` is invalid; use `erase` to remove an order.
The book does not decide whether quantity increases should lose priority. That
policy belongs outside this storage component.

`erase` removes an active order by `OrderId` from the FIFO, ID index, and pool.
The result contains a value snapshot of the removed order.

## Memory Ownership

Construction allocates exactly these fixed arrays:

- one `Order[]` in `OrderPool`;
- one bucket array in `OrderIdIndex`;
- one `PriceSegment[]` array for bids;
- one `PriceSegment[]` array for asks.

After construction, runtime operations `insert`, `best`, `set_remaining`, and
`erase` do not allocate. `validate_invariants()` is a slow debug/test helper and
may allocate temporary memory.

## Failure Atomicity

Failed `insert` paths leave the logical book unchanged for:

- duplicate `OrderId`;
- capacity exhaustion;
- price out of range;
- invalid quantity.

Failed `set_remaining(NotFound/InvalidQuantity)` and failed `erase(NotFound)`
also leave the logical book unchanged.

## Index Strategy

`OrderIdIndex` uses fixed open addressing over a preallocated power-of-two bucket
array. Deletion compacts the affected probe cluster in-place by reinserting
following occupied buckets into the same array. The index has only Empty and
Occupied states and does not allocate or runtime-rehash.

Probe diagnostics are available for tests and benchmarks through overloads that
accept `IndexProbeStats*`; normal runtime calls do not collect stats.

## First Touch

Constructors allocate and sequentially initialize the order pool, ID index, and
bid/ask side storage. Construct `OrderBook` on the final matcher/owner thread
after CPU affinity and NUMA policy have already been selected if first-touch
placement matters.

The library does not call `mlockall`, `VirtualLock`, thread affinity, or NUMA
policy APIs. OS memory locking remains a platform/runtime responsibility.

## Thread Safety

Not thread-safe. All access must be externally serialized by the single owner.
The component contains no mutexes, atomics, or lock-free structures.

## Benchmark Methodology

Benchmarks use repeated batches and report batch-normalized `ns/op`. Percentiles
(`p50`, `p90`, `p99`, `p99.9`, `p99.99`, `max`) are percentiles of batch samples,
not hardware-timed individual operations. Setup and construction are outside
measured hot-path sections unless a scenario explicitly measures construction.
Benchmark does not set OS affinity or memory locking.

## Known Limitations

- No matcher, no execution policy, no event output.
- Linux portability is expected from the C++20 code and CMake, but was not
  verified in this local environment.
