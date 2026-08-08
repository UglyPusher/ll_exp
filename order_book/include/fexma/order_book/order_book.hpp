/**
 * @file order_book.hpp
 * @brief Public facade for standalone resting-order storage.
 *
 * OrderBook owns bid/ask SideBook instances, an internal fixed order pool, and
 * a fixed OrderId index. It does not implement matching policy, trade events,
 * WAL, account state, callbacks, locks, atomics, or networking.
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
 * Runtime mutation methods do not allocate after construction.
 * validate_invariants() is a debug/test helper and may allocate.
 *
 * @note This class is not a matcher and does not implement execution policy,
 * STP, events, WAL, client validation, or session state.
 * @warning Not thread-safe. All access must be externally serialized.
 */
class OrderBook final {
  friend class OrderBookTestAccess;

public:
  /** @brief Allocates all fixed storage for the configured range/capacity. */
  explicit OrderBook(const OrderBookConfig& config);

  OrderBook(const OrderBook&) = delete;
  OrderBook& operator=(const OrderBook&) = delete;
  OrderBook(OrderBook&&) noexcept = default;
  OrderBook& operator=(OrderBook&&) noexcept = default;

  /**
   * @brief Appends one resting order to its side/price FIFO.
   *
   * @return Explicit status. On failure, logical book state remains unchanged.
   * @complexity Expected O(1), bounded by fixed index probe length.
   */
  [[nodiscard]] InsertResult insert(const RestingOrderData& order) noexcept;
  /**
   * @brief Returns the best resting order on one side.
   *
   * @return Snapshot of the best resting order, or std::nullopt if the side is
   * empty. The operation does not create hidden selection state.
   */
  [[nodiscard]] std::optional<OrderView> best(Side side) const noexcept;
  /**
   * @brief Removes an active order by OrderId.
   *
   * @return Ok includes a snapshot of the removed order. NotFound leaves book
   * state unchanged.
   */
  [[nodiscard]] EraseResult erase(OrderId id) noexcept;
  /**
   * @brief Sets remaining quantity by OrderId without changing FIFO priority.
   *
   * @return Ok includes the previous quantity. InvalidQuantity and NotFound
   * leave book state unchanged.
   */
  [[nodiscard]] SetRemainingResult
  set_remaining(OrderId id, Quantity new_remaining) noexcept;

  /**
   * @brief Slow structural verifier for tests and Debug diagnostics.
   *
   * @note May allocate temporary memory and is not part of the hot path.
   */
  [[nodiscard]] bool validate_invariants() const noexcept;

private:
  using BidBook = SideBook<Side::Bid>;
  using AskBook = SideBook<Side::Ask>;

  [[nodiscard]] std::uint32_t active_order_count() const noexcept;
  [[nodiscard]] bool price_in_range(PriceTick price) const noexcept;
  [[nodiscard]] OrderView view_for(OrderIndex slot) const noexcept;
  [[nodiscard]] OrderView remove_active_order(OrderIndex slot) noexcept;
  [[nodiscard]] bool validate_side(Side side) const noexcept;

  OrderBookConfig config_;
  detail::OrderPool pool_;
  OrderIdIndex index_;
  BidBook bids_;
  AskBook asks_;
};

} // namespace fexma::order_book
