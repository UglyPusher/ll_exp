/**
 * @file test_price_structures.cpp
 * @brief Autonomous tests for PriceSegment masks and SideBook FIFO behavior.
 */
#include <fexma/order_book/side_book.hpp>

using namespace fexma::order_book;

namespace {

using detail::OrderPool;

OrderIndex make_order(OrderPool& pool, OrderId id, Side side, PriceTick price,
                      Quantity quantity) {
  return pool.emplace(id, id + 1000, price, quantity, side);
}

} // namespace

int main() {
  PriceSegment segment;
  segment.active_mask = (std::uint64_t{1} << 5) | (std::uint64_t{1} << 9);
  if (segment.best_ask_offset() != 5 || segment.best_bid_offset() != 9) {
    return 1;
  }
  segment.active_mask &= ~(std::uint64_t{1} << 5);
  if (segment.best_ask_offset() != 9 || segment.best_bid_offset() != 9) {
    return 2;
  }
  segment.active_mask = 0;
  if (!segment.empty()) {
    return 3;
  }

  OrderPool pool(16);
  SideBook<Side::Ask> asks(64, 191);

  const OrderIndex ask_high = make_order(pool, 1, Side::Ask, 130, 10);
  const OrderIndex ask_low_a = make_order(pool, 2, Side::Ask, 70, 20);
  const OrderIndex ask_low_b = make_order(pool, 3, Side::Ask, 70, 30);
  asks.append(pool, ask_high);
  asks.append(pool, ask_low_a);
  asks.append(pool, ask_low_b);

  if (asks.best_price() != 70 || asks.best_order(pool) != ask_low_a) {
    return 4;
  }
  asks.reduce(pool, ask_low_a, 5);
  if (pool.get_unchecked(ask_low_a).remaining != 15) {
    return 5;
  }
  asks.remove(pool, ask_low_a);
  if (asks.best_order(pool) != ask_low_b) {
    return 6;
  }
  asks.remove(pool, ask_low_b);
  if (asks.best_price() != 130) {
    return 7;
  }
  asks.remove(pool, ask_high);
  if (!asks.empty()) {
    return 8;
  }

  SideBook<Side::Bid> bids(64, 191);
  const OrderIndex bid_low = make_order(pool, 4, Side::Bid, 65, 10);
  const OrderIndex bid_high = make_order(pool, 5, Side::Bid, 190, 10);
  const OrderIndex bid_mid = make_order(pool, 6, Side::Bid, 128, 10);
  bids.append(pool, bid_low);
  bids.append(pool, bid_high);
  bids.append(pool, bid_mid);
  if (bids.best_price() != 190 || bids.best_order(pool) != bid_high) {
    return 9;
  }
  bids.remove(pool, bid_high);
  if (bids.best_price() != 128) {
    return 10;
  }
  bids.remove(pool, bid_mid);
  if (bids.best_price() != 65) {
    return 11;
  }

  return 0;
}
