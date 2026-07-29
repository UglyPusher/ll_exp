/**
 * @file price_level.hpp
 * @brief FIFO aggregate for one active price.
 *
 * PriceLevel stores only slot links and aggregates. Orders themselves live in
 * OrderPool; FIFO membership is intrusive via Order::prev/next slot indices.
 */
#pragma once

#include <cstdint>

#include <fexma/order_book/types.hpp>

namespace fexma::order_book {

/** @brief Intrusive FIFO queue and aggregate counters for one price tick. */
struct PriceLevel {
  OrderSlot head{invalid_order_slot};
  OrderSlot tail{invalid_order_slot};
  Quantity total_quantity{};
  std::uint32_t order_count{};

  [[nodiscard]] bool empty() const noexcept {
    return order_count == 0;
  }
};

} // namespace fexma::order_book
