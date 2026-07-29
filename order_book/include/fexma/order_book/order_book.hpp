#pragma once

#include <cassert>
#include <cstddef>
#include <optional>

#include <fexma/order_book/order_id_index.hpp>
#include <fexma/order_book/side_book.hpp>

namespace fexma::order_book {

class OrderBook {
public:
  explicit OrderBook(const OrderBookConfig& config);

  OrderBook(const OrderBook&) = delete;
  OrderBook& operator=(const OrderBook&) = delete;
  OrderBook(OrderBook&&) noexcept = default;
  OrderBook& operator=(OrderBook&&) noexcept = default;

  void warm_up() noexcept;

  [[nodiscard]] PutResult put(const RestingOrderData& order) noexcept;
  [[nodiscard]] std::optional<BestOrderView>
  select_best_opposite(Side incoming_side) noexcept;
  void decrement_selected(Quantity quantity) noexcept;
  [[nodiscard]] CancelResult cancel(OrderId id) noexcept;
  [[nodiscard]] ChangeResult change(OrderId id,
                                     const OrderChange& change) noexcept;

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

#ifdef FEXMA_ORDER_BOOK_ENABLE_TEST_ACCESS
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
};

} // namespace fexma::order_book
