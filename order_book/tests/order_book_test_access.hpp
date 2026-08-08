/**
 * @file order_book_test_access.hpp
 * @brief Test-only corruption hooks for OrderBook invariant tests.
 */
#pragma once

#include <fexma/order_book/order_book.hpp>

namespace fexma::order_book {

class OrderBookTestAccess {
public:
  static void set_remaining(OrderBook& book, OrderId id,
                            Quantity remaining) noexcept {
    const OrderIndex slot = book.index_.find(id);
    book.pool_.get_unchecked(slot).remaining = remaining;
  }

  static void set_price(OrderBook& book, OrderId id, PriceTick price) noexcept {
    const OrderIndex slot = book.index_.find(id);
    book.pool_.get_unchecked(slot).price = price;
  }

  static void set_best_segment(OrderBook& book, Side side,
                               std::size_t segment) noexcept {
    if (side == Side::Bid) {
      book.bids_.best_segment_ = segment;
    } else {
      book.asks_.best_segment_ = segment;
    }
  }
};

} // namespace fexma::order_book
