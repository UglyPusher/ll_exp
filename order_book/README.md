# fexma::order_book

Standalone single-threaded storage for resting orders in a low-latency matching
engine. The library owns bid/ask books, intrusive FIFO price levels, a fixed
order pool, and a fixed-capacity `OrderId -> OrderSlot` index.

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

After construction and `warm_up()`, runtime operations `put`, `select`, `decrement`,
`cancel`, and `change` do not allocate. `validate_invariants()` is explicitly a
slow debug/test helper and may allocate temporary memory.

## Selected-Order Protocol

The stateful protocol is:

```text
select_best_opposite()
matcher reads BestOrderView snapshot
decrement_selected()
```

A successful `select_best_opposite()` caches an internal slot and generation.
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
and ask segments through volatile page touches, then reinitializes structures.

`warm_up()` must be called before runtime orders are inserted. Calling it after
runtime start would reset the book; Debug builds assert that the book is empty.
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

Environment checked: MSVC 2022 x64, Release build, Windows CMake from Visual
Studio. GCC/Clang Linux builds were not run in this environment.

Tests:

```text
Release: 100% tests passed, 0 tests failed out of 5
Debug:   100% tests passed, 0 tests failed out of 5
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

Selected benchmark results from the latest run:

```text
pool_acquire mean=6.0788 ns/op p99=47.4
pool_release mean=24.393 ns/op p99=30
index_find_hit_load_50 mean=12.5381 ns/op avg_probes=1.001 max_probes=2
index_find_miss_load_85 mean=166.771 ns/op avg_probes=22.366 max_probes=318
index_churn_after_1M_insert_erase mean=88.3759 ns/op tombstones=0
select_best_only mean=9.0579 ns/op p99=17
decrement_partial_only mean=21.2347 ns/op p99=49.6
full_remove_keep_price_level_nonempty mean=260.704 ns/op p99=419.7
full_remove_empty_price_level_same_segment mean=268.638 ns/op p99=424.1
full_remove_cross_segment mean=217.46 ns/op p99=400
cancel_head mean=260.294 ns/op p99=433.4
change_quantity mean=160.809 ns/op p99=826.6
```

## Known Limitations

- No matcher, no execution policy, no event output.
- `change` cannot increase quantity or change price.
- `warm_up()` is pre-runtime only.
- `decrement_selected()` reports protocol errors by Debug assertions because the
  API is intentionally void.
- Linux portability is expected from the C++20 code and CMake, but was not
  verified in this local environment.
