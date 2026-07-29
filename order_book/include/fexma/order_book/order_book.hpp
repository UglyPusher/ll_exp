/**
 * @file order_book.hpp
 * @brief Public facade for standalone resting-order storage.
 *
 * OrderBook owns bid/ask SideBook instances, a fixed OrderPool, and a fixed
 * OrderId index. It does not implement matching policy, trade events, WAL,
 * account state, callbacks, locks, atomics, or networking.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

#include <fexma/order_book/order_id_index.hpp>
#include <fexma/order_book/side_book.hpp>

namespace fexma::order_book {

/**
 * @brief Single-writer in-memory storage for resting bid and ask orders.
 *
 * The book owns all resting orders, FIFO price levels, price segments, the
 * fixed-capacity order pool, and the OrderId index.
 *
 * Runtime mutation methods do not allocate after construction and warm-up.
 * validate_invariants() is a debug/test helper and may allocate.
 *
 * @note This class is not a matcher and does not implement execution policy,
 * STP, events, WAL, client validation, or session state.
 * @warning Not thread-safe. All access must be externally serialized.
 */
class OrderBook {
public:
  /** @brief Allocates all fixed storage for the configured range/capacity. */
  explicit OrderBook(const OrderBookConfig& config);

  OrderBook(const OrderBook&) = delete;
  OrderBook& operator=(const OrderBook&) = delete;
  OrderBook(OrderBook&&) noexcept = default;
  OrderBook& operator=(OrderBook&&) noexcept = default;

  /**
   * @brief Touches and reinitializes all owned fixed storage.
   *
   * @pre Must be called before runtime orders are inserted. Calling warm_up()
   * after the book becomes active is a contract violation; Debug builds assert.
   * @post The book is empty and selection is invalid.
   * @note No OS-specific memory locking or affinity is performed.
   * @complexity O(max_orders + segment_count + index_bucket_count).
   */
  void warm_up() noexcept;

  /**
   * @brief Appends one resting order to its side/price FIFO.
   *
   * @return Explicit status. On failure, logical book state and current
   * selection remain unchanged.
   * @post On success, any previous selection is invalidated.
   * @complexity Expected O(1), bounded by fixed index probe length.
   */
  [[nodiscard]] PutResult put(const RestingOrderData& order) noexcept;
  /**
   * @brief Selects and caches the best resting order on the opposite side.
   *
   * @param incoming_side Side of the incoming order.
   * @return Snapshot of the selected resting order, or std::nullopt if the
   * opposite side is empty.
   * @post A successful call caches an internal selected-order slot. A later
   * select replaces the previous selection. Empty selection clears it.
   * @warning Any successful put/cancel/change invalidates the current selection.
   */
  [[nodiscard]] std::optional<BestOrderView>
  select_best_opposite(Side incoming_side) noexcept;
  /**
   * @brief Reduces the currently selected order.
   *
   * @pre A previous select_best_opposite() returned a value.
   * @pre quantity > 0 and quantity <= selected.remaining.
   * @post Selection is cleared. If remaining reaches zero, the order is removed
   * from FIFO, index, and pool.
   * @warning Protocol violations are checked with assert in Debug. Release
   * builds rely on the documented preconditions because the API is void.
   */
  void decrement_selected(Quantity quantity) noexcept;
  /**
   * @brief Removes an active order by OrderId.
   *
   * @return NotFound leaves book state and current selection unchanged. Ok
   * invalidates any selection.
   */
  [[nodiscard]] CancelResult cancel(OrderId id) noexcept;
  /**
   * @brief Reduces remaining quantity by OrderId without changing FIFO priority.
   *
   * @return InvalidQuantity/NotFound leave book state and selection unchanged.
   * new_remaining == 0 is structurally equivalent to cancel().
   */
  [[nodiscard]] ChangeResult change(OrderId id,
                                     const OrderChange& change) noexcept;

  /**
   * @brief Slow structural verifier for tests and Debug diagnostics.
   *
   * @note May allocate temporary memory and is not part of the hot path.
   */
  [[nodiscard]] bool validate_invariants() const noexcept;

  [[nodiscard]] bool empty() const noexcept {
    return active_order_count() == 0;
  }

  [[nodiscard]] std::uint32_t active_order_count() const noexcept {
    return bids_.order_count() + asks_.order_count();
  }

  [[nodiscard]] OrderSlot capacity() const noexcept {
    return pool_.capacity();
  }

  [[nodiscard]] std::optional<PriceTick> best_bid() const noexcept;
  [[nodiscard]] std::optional<PriceTick> best_ask() const noexcept;

  [[nodiscard]] WarmUpTouchStats pool_warm_up_stats() const noexcept {
    return pool_.last_warm_up_stats();
  }

  [[nodiscard]] WarmUpTouchStats index_warm_up_stats() const noexcept {
    return index_.last_warm_up_stats();
  }

  [[nodiscard]] WarmUpTouchStats bid_warm_up_stats() const noexcept {
    return bids_.last_warm_up_stats();
  }

  [[nodiscard]] WarmUpTouchStats ask_warm_up_stats() const noexcept {
    return asks_.last_warm_up_stats();
  }

  [[nodiscard]] std::size_t index_bucket_count() const noexcept {
    return index_.bucket_count();
  }

  [[nodiscard]] std::size_t segment_count() const noexcept {
    return bids_.segment_count();
  }

#ifdef FEXMA_ORDER_BOOK_ENABLE_TEST_ACCESS
  [[nodiscard]] OrderSlot free_count_for_test() const noexcept {
    return pool_.free_count();
  }

  [[nodiscard]] OrderSlot selected_order_for_test() const noexcept {
    return selected_order_;
  }

  [[nodiscard]] std::uint64_t generation_for_test() const noexcept {
    return generation_;
  }

  [[nodiscard]] const PriceSegment& segment_for_test(
      Side side, std::size_t index) const noexcept {
    return side == Side::Bid ? bids_.segment(index) : asks_.segment(index);
  }

  template <typename Fn>
  bool for_each_order_for_test(Side side, Fn&& fn) const noexcept {
    const auto walk_asks = [this, &fn](const auto& book) noexcept {
      for (std::size_t segment_index = 0; segment_index < book.segment_count();
           ++segment_index) {
        const PriceSegment& segment = book.segment(segment_index);
        for (std::uint32_t offset = 0; offset < prices_per_segment; ++offset) {
          for (OrderSlot slot = segment.levels[offset].head;
               slot != invalid_order_slot; slot = pool_[slot].next) {
            const Order& order = pool_[slot];
            if (!fn(BestOrderView{order.id, order.owner_id, order.price,
                                  order.remaining})) {
              return false;
            }
          }
        }
      }
      return true;
    };

    const auto walk_bids = [this, &fn](const auto& book) noexcept {
      for (std::size_t segment_index = book.segment_count(); segment_index > 0;
           --segment_index) {
        const PriceSegment& segment = book.segment(segment_index - 1);
        for (std::uint32_t offset = prices_per_segment; offset > 0; --offset) {
          for (OrderSlot slot = segment.levels[offset - 1].head;
               slot != invalid_order_slot; slot = pool_[slot].next) {
            const Order& order = pool_[slot];
            if (!fn(BestOrderView{order.id, order.owner_id, order.price,
                                  order.remaining})) {
              return false;
            }
          }
        }
      }
      return true;
    };

    return side == Side::Bid ? walk_bids(bids_) : walk_asks(asks_);
  }
#endif

private:
  using BidBook = SideBook<Side::Bid>;
  using AskBook = SideBook<Side::Ask>;

  [[nodiscard]] bool price_in_range(PriceTick price) const noexcept;
  void invalidate_selection() noexcept;
  void mark_mutation() noexcept;
  void remove_active_order(OrderSlot slot) noexcept;
  [[nodiscard]] bool slot_in_any_fifo(OrderSlot slot) const noexcept;
  [[nodiscard]] bool slot_in_freelist(OrderSlot slot) const noexcept;
  [[nodiscard]] bool validate_side(Side side) const noexcept;

  OrderBookConfig config_;
  OrderPool pool_;
  OrderIdIndex index_;
  BidBook bids_;
  AskBook asks_;
  OrderSlot selected_order_{invalid_order_slot};
  std::uint64_t generation_{};
  std::uint64_t selected_generation_{};
};

} // namespace fexma::order_book
