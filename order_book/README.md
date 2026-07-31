# fexma::order_book

Standalone single-threaded storage for resting orders in a low-latency matching
engine. The library owns bid/ask books, intrusive FIFO price levels, a fixed
order pool, and a fixed-capacity `OrderId -> OrderIndex` index.

It deliberately does not implement matcher policy, STP, events, WAL, accounts,
networking, callbacks, virtual interfaces, locks, atomics, or session state.

## Public API

- `OrderBook(const OrderBookConfig& config)`
- `void warm_up() noexcept`
- `PutResult put(const RestingOrderData& order) noexcept`
- `std::optional<BestOrderView> select_best_opposite(Side incoming_side) noexcept`
- `void decrement_selected(Quantity quantity) noexcept`
- `CancelResult cancel(OrderId id) noexcept`
- `ChangeResult change(OrderId id, const OrderChange& change) noexcept`
- `bool validate_invariants() const noexcept`
- `std::optional<PriceTick> best_bid() const noexcept`
- `std::optional<PriceTick> best_ask() const noexcept`

`change` supports only quantity reduction. `new_remaining == 0` is structurally
equivalent to `cancel`. Price changes and quantity increases belong to external
cancel plus put logic.

## Memory Ownership

Construction allocates exactly these fixed arrays:

- one `Order[]` in `OrderPool`;
- one bucket array in `OrderIdIndex`;
- one `PriceSegment[]` array for bids;
- one `PriceSegment[]` array for asks.

After construction and `warm_up()`, runtime operations `put`, `select`,
`decrement`, `cancel`, and `change` do not allocate. `validate_invariants()` is
explicitly a slow debug/test helper and may allocate temporary memory.

## Selected-Order Protocol

The stateful protocol is:

```text
select_best_opposite()
matcher reads BestOrderView snapshot
decrement_selected()
```

A successful `select_best_opposite()` caches an internal order-pool index and generation.
Calling `select_best_opposite()` again replaces the selection. Selecting an empty
opposite side clears it. Any successful `put`, `cancel`, or `change` invalidates
selection. Failed `put`, `cancel`, or `change` calls leave selection unchanged.

`decrement_selected()` is intentionally `void`; protocol violations are Debug
assertions and Release builds rely on documented preconditions.

## Failure Atomicity

Failed `put` paths leave the logical book unchanged for:

- duplicate `OrderId`;
- pool exhaustion;
- index full;
- price out of range;
- invalid quantity.

Failed `cancel(NotFound)` and failed `change(NotFound/InvalidQuantity)` also
leave the logical book and selected-order state unchanged.

## Index Strategy

`OrderIdIndex` uses fixed open addressing over a preallocated power-of-two bucket
array. Deletion compacts the affected probe cluster in-place by reinserting
following occupied buckets into the same array. This avoids tombstone accumulation
and does not allocate or runtime-rehash.

Probe diagnostics are available for tests and benchmarks through overloads that
accept `IndexProbeStats*`; normal runtime calls do not collect stats.

## Warm-Up

`warm_up()` touches the pages for the order pool, index buckets, bid segments,
and ask segments through volatile page touches. `OrderPool::prefault_pages()`
does not change logical pool state; explicit `OrderPool::reset()` is the
operation that rebuilds the freelist and discards contents.

`OrderPool(capacity)` remains a ready-to-use constructor and therefore builds
the freelist immediately. `OrderBook` uses the explicit
`OrderPool::Uninitialized` construction path instead, so pool pages are not
first-touched until `OrderBook::warm_up()` runs on the caller's chosen thread.

`warm_up()` must be called before runtime orders are inserted because the side
books and index still clear their fixed storage; Debug builds assert that the
book is empty.
The library does not call `mlockall`, `VirtualLock`, thread affinity, or NUMA
policy APIs.

## Thread Safety

Not thread-safe. All access must be externally serialized by the single owner.
The component contains no mutexes, atomics, or lock-free structures.

## Benchmark Methodology

Benchmarks use repeated batches and report batch-normalized `ns/op`. Percentiles
(`p50`, `p90`, `p99`, `p99.9`, `p99.99`, `max`) are percentiles of batch samples,
not hardware-timed individual operations. Initialization and warm-up are outside
measured sections. Benchmark does not set OS affinity or memory locking.

## Latest Local Run

Environment checked: MSVC 2022 x64, direct `cl` optimized build. GCC/Clang Linux
builds were not run in this environment.

Tests:

```text
/O2 /DNDEBUG: 5/5 tests passed
```

Memory layout report:

```text
sizeof(Order)=40 alignof(Order)=8
sizeof(PriceLevel)=16 alignof(PriceLevel)=4
sizeof(PriceSegment)=1032 alignof(PriceSegment)=8
sizeof(OrderIdIndex::Bucket)=16 alignof(OrderIdIndex::Bucket)=8
max_orders=100000 min_price_tick=1 max_price_tick=4096
segment_count=65 index_capacity=262144 index_load_factor=0.38147
OrderPool_bytes=4000000 OrderIdIndex_bytes=4194304
Bid_segments_bytes=67080 Ask_segments_bytes=67080 Total_bytes=8328464
```

Selected `bench_order_pool` results, median of five direct `/O2 /DNDEBUG` runs:

```text
pool_allocate_uninitialized median_ms=0.052 best_ms=0.0381 worst_ms=0.1201
pool_prefault median_ms=24.5196 best_ms=17.5701 worst_ms=47.3211
pool_reset median_ms=27.6719 best_ms=18.4878 worst_ms=43.7141
pool_bulk_acquire_blocks block=64 median_mean=32.4688 ns/op median_p99=3600 ns/block
pool_bulk_release_blocks block=64 median_mean=28.6712 ns/op median_p99=3000 ns/block
pool_bulk_acquire_blocks block=4096 median_mean=33.6575 ns/op median_p99=579100 ns/block
pool_bulk_release_blocks block=4096 median_mean=24.9241 ns/op median_p99=362900 ns/block
pool_bulk_acquire_blocks block=65536 median_mean=30.5866 ns/op median_p99=3350800 ns/block
pool_bulk_release_blocks block=65536 median_mean=25.58 ns/op median_p99=2473200 ns/block
```

## Known Limitations

- No matcher, no execution policy, no event output.
- `change` cannot increase quantity or change price.
- `warm_up()` is pre-runtime only for the full `OrderBook`.
- `decrement_selected()` reports protocol errors by Debug assertions because the
  API is intentionally void.
- Linux portability is expected from the C++20 code and CMake, but was not
  verified in this local environment.
