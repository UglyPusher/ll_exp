/**
 * @file test_price_structures.cpp
 * @brief Autonomous tests for PriceSegment masks and SideBook FIFO behavior.
 */
#include <fexma/order_book/side_book.hpp>

#include <limits>
#include <type_traits>

using namespace fexma::order_book;

namespace {

using detail::OrderPool;

static_assert(!std::is_default_constructible_v<SideBook<Side::Ask>>);
static_assert(!std::is_default_constructible_v<SideBook<Side::Bid>>);

OrderIndex make_order(OrderPool& pool, OrderId id, Side side, PriceTick price,
                      Quantity quantity) {
  return pool.emplace(id, id + 1000, price, quantity, side);
}

template <Side BookSide>
bool segment_occupied(const SideBook<BookSide>& book,
                      std::size_t segment_index) {
  const std::size_t word_index = segment_index >> 6U;
  const std::uint64_t bit = std::uint64_t{1} << (segment_index & 63U);
  return (book.segment_occupancy_word(word_index) & bit) != 0;
}

bool segment_occupancy_tracks_empty_transitions() {
  OrderPool pool(4);
  SideBook<Side::Ask> asks(0, 127);
  const OrderIndex low = make_order(pool, 10, Side::Ask, 3, 10);
  const OrderIndex high = make_order(pool, 11, Side::Ask, 7, 10);

  asks.append(pool, low);
  asks.append(pool, high);
  if (!segment_occupied(asks, 0)) {
    return false;
  }

  asks.remove(pool, low);
  if (!segment_occupied(asks, 0) || asks.best_price() != 7) {
    return false;
  }

  asks.remove(pool, high);
  return !segment_occupied(asks, 0) && asks.empty() &&
         asks.best_order() == invalid_order_index;
}

bool segment_occupancy_crosses_word_boundaries() {
  OrderPool pool(4);
  SideBook<Side::Ask> asks(0, 8191);
  const OrderIndex segment_63 =
      make_order(pool, 20, Side::Ask, 63 * prices_per_segment, 10);
  const OrderIndex segment_64 =
      make_order(pool, 21, Side::Ask, 64 * prices_per_segment, 10);

  asks.append(pool, segment_64);
  asks.append(pool, segment_63);
  if (!segment_occupied(asks, 63) || !segment_occupied(asks, 64) ||
      asks.best_price() != 63 * prices_per_segment) {
    return false;
  }

  asks.remove(pool, segment_63);
  if (segment_occupied(asks, 63) || !segment_occupied(asks, 64) ||
      asks.best_price() != 64 * prices_per_segment) {
    return false;
  }

  asks.remove(pool, segment_64);
  return !segment_occupied(asks, 64) && asks.empty();
}

bool best_recompute_handles_near_and_far_for_asks() {
  OrderPool pool(4);
  SideBook<Side::Ask> asks(0, 8191);
  const OrderIndex near = make_order(pool, 30, Side::Ask, 0, 10);
  const OrderIndex next_near =
      make_order(pool, 31, Side::Ask, prices_per_segment, 10);
  const OrderIndex far =
      make_order(pool, 32, Side::Ask, 127 * prices_per_segment, 10);

  asks.append(pool, far);
  asks.append(pool, next_near);
  asks.append(pool, near);
  asks.remove(pool, near);
  if (asks.best_price() != prices_per_segment) {
    return false;
  }
  asks.remove(pool, next_near);
  return asks.best_price() == 127 * prices_per_segment;
}

bool best_recompute_handles_near_and_far_for_bids() {
  OrderPool pool(4);
  SideBook<Side::Bid> bids(0, 8191);
  const OrderIndex low = make_order(pool, 40, Side::Bid, 0, 10);
  const OrderIndex next_high =
      make_order(pool, 41, Side::Bid, 126 * prices_per_segment, 10);
  const OrderIndex high =
      make_order(pool, 42, Side::Bid, 127 * prices_per_segment, 10);

  bids.append(pool, low);
  bids.append(pool, next_high);
  bids.append(pool, high);
  bids.remove(pool, high);
  if (bids.best_price() != 126 * prices_per_segment) {
    return false;
  }
  bids.remove(pool, next_high);
  return bids.best_price() == 0;
}

bool removing_only_level_clears_segment() {
  OrderPool pool(2);
  SideBook<Side::Bid> bids(0, 127);
  const OrderIndex order = make_order(pool, 50, Side::Bid, 64, 10);

  bids.append(pool, order);
  if (!segment_occupied(bids, 1) || bids.best_price() != 64) {
    return false;
  }

  bids.remove(pool, order);
  return !segment_occupied(bids, 1) && bids.empty() &&
         bids.best_order() == invalid_order_index;
}

bool empty_side_has_empty_segment_bitmap() {
  OrderPool pool(1);
  SideBook<Side::Ask> asks(0, 127);
  return asks.best_order() == invalid_order_index &&
         asks.segment_occupancy_word_count() == 1 &&
         asks.segment_occupancy_word(0) == 0;
}

bool fifo_unlink_updates_aggregates_once() {
  OrderPool pool(8);
  SideBook<Side::Ask> asks(0, 127);
  const OrderIndex head = make_order(pool, 60, Side::Ask, 42, 10);
  const OrderIndex middle = make_order(pool, 61, Side::Ask, 42, 20);
  const OrderIndex tail = make_order(pool, 62, Side::Ask, 42, 30);

  asks.append(pool, head);
  asks.append(pool, middle);
  asks.append(pool, tail);

  const PriceLevel& level = asks.segment(0).levels[42];
  if (asks.order_count() != 3 || asks.total_quantity() != 60 ||
      level.order_count != 3 || level.total_quantity != 60 ||
      level.head != head || level.tail != tail) {
    return false;
  }

  asks.remove(pool, middle);
  if (asks.order_count() != 2 || asks.total_quantity() != 40 ||
      level.order_count != 2 || level.total_quantity != 40 ||
      level.head != head || level.tail != tail ||
      pool.get_unchecked(head).next != tail ||
      pool.get_unchecked(tail).prev != head) {
    return false;
  }

  asks.remove(pool, head);
  if (asks.order_count() != 1 || asks.total_quantity() != 30 ||
      level.order_count != 1 || level.total_quantity != 30 ||
      level.head != tail || level.tail != tail ||
      pool.get_unchecked(tail).prev != invalid_order_index) {
    return false;
  }

  asks.remove(pool, tail);
  return asks.empty() && asks.total_quantity() == 0 &&
         level.order_count == 0 && level.total_quantity == 0 &&
         level.head == invalid_order_index &&
         level.tail == invalid_order_index &&
         asks.segment(0).active_mask == 0 &&
         !segment_occupied(asks, 0);
}

bool released_slot_reuse_has_fresh_fifo_links() {
  OrderPool pool(2);
  SideBook<Side::Ask> asks(0, 127);
  SideBook<Side::Bid> bids(0, 127);

  const OrderIndex first = make_order(pool, 70, Side::Ask, 10, 15);
  asks.append(pool, first);
  asks.remove(pool, first);
  pool.release(first);

  const OrderIndex reused = make_order(pool, 71, Side::Bid, 90, 25);
  if (reused != first) {
    return false;
  }

  const detail::Order& reused_order = pool.get_unchecked(reused);
  if (reused_order.id != 71 || reused_order.owner_id != 1071 ||
      reused_order.price != 90 || reused_order.remaining != 25 ||
      reused_order.side != Side::Bid ||
      reused_order.prev != invalid_order_index ||
      reused_order.next != invalid_order_index) {
    return false;
  }

  bids.append(pool, reused);
  return asks.empty() && bids.best_order() == reused &&
         bids.best_price() == 90 && bids.order_count() == 1 &&
         bids.total_quantity() == 25;
}

bool aggregate_quantity_exceeds_uint32_without_overflow() {
  OrderPool pool(2);
  SideBook<Side::Ask> asks(0, 127);
  constexpr Quantity maximum_quantity =
      (std::numeric_limits<Quantity>::max)();
  const OrderIndex first =
      make_order(pool, 80, Side::Ask, 42, maximum_quantity);
  const OrderIndex second =
      make_order(pool, 81, Side::Ask, 42, maximum_quantity);

  asks.append(pool, first);
  asks.append(pool, second);
  const AggregateQuantity expected =
      static_cast<AggregateQuantity>(maximum_quantity) * 2U;
  const PriceLevel& level = asks.segment(0).levels[42];
  if (asks.total_quantity() != expected || level.total_quantity != expected) {
    return false;
  }

  asks.set_remaining(pool, first, maximum_quantity - 1U);
  return asks.total_quantity() == expected - 1U &&
         level.total_quantity == expected - 1U;
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

  if (asks.best_price() != 70 || asks.best_order() != ask_low_a) {
    return 4;
  }
  asks.set_remaining(pool, ask_low_a, 15);
  if (pool.get_unchecked(ask_low_a).remaining != 15) {
    return 5;
  }
  asks.remove(pool, ask_low_a);
  if (asks.best_order() != ask_low_b) {
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
  if (bids.best_price() != 190 || bids.best_order() != bid_high) {
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

  if (!segment_occupancy_tracks_empty_transitions()) {
    return 12;
  }
  if (!segment_occupancy_crosses_word_boundaries()) {
    return 13;
  }
  if (!best_recompute_handles_near_and_far_for_asks()) {
    return 14;
  }
  if (!best_recompute_handles_near_and_far_for_bids()) {
    return 15;
  }
  if (!removing_only_level_clears_segment()) {
    return 16;
  }
  if (!empty_side_has_empty_segment_bitmap()) {
    return 17;
  }
  if (!fifo_unlink_updates_aggregates_once()) {
    return 18;
  }
  if (!released_slot_reuse_has_fresh_fifo_links()) {
    return 19;
  }
  if (!aggregate_quantity_exceeds_uint32_without_overflow()) {
    return 20;
  }

  return 0;
}
