# fexma::order_book

Standalone single-threaded resting order storage for a low-latency matching
engine. The library owns bid/ask books, intrusive FIFO price levels, a fixed
order pool, and a fixed-capacity `OrderId -> OrderSlot` index.

The subsystem deliberately does not implement matching policy, events, WAL,
accounts, networking, session state, callbacks, locks, atomics, or virtual
interfaces.

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

`change` currently supports only quantity reduction. Price changes and quantity
increases are intentionally left to external cancel plus put logic.

## Memory

Initialization allocates:

- one `Order[]` array in `OrderPool`;
- one fixed bucket array in `OrderIdIndex`;
- one fixed `PriceSegment[]` array for bids;
- one fixed `PriceSegment[]` array for asks.

After `warm_up()`, normal runtime operations do not allocate and do not grow
any container. `validate_invariants()` is a debug/test helper and may allocate
temporary memory while checking the full structure.
