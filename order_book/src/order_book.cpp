/**
 * @file order_book.cpp
 * @brief OrderBook facade implementation and slow invariant validation.
 *
 * The implementation coordinates the fixed pool, fixed OrderId index, and the
 * bid/ask side books. It keeps structural operations local and deliberately
 * avoids matcher policy, event generation, WAL, callbacks, locks, and atomics.
 */
#include <fexma/order_book/order_book.hpp>

#include <memory>

namespace fexma::order_book {

OrderBook::OrderBook(const OrderBookConfig& config)
    : config_(config),
      pool_(config.max_orders),
      index_(config.max_orders),
      bids_(config.min_price_tick, config.max_price_tick),
      asks_(config.min_price_tick, config.max_price_tick) {}

InsertResult OrderBook::insert(const RestingOrderData& order) noexcept {
  // Failure checks happen before any mutation so rejected puts preserve the
  // logical book state.
  if (order.quantity == 0) {
    return {InsertStatus::InvalidQuantity};
  }
  if (!price_in_range(order.price)) {
    return {InsertStatus::PriceOutOfRange};
  }
  if (index_.find(order.id) != invalid_order_index) {
    return {InsertStatus::DuplicateOrderId};
  }

  const OrderIndex slot = pool_.emplace(order.id, order.owner_id, order.price,
                                        order.quantity, order.side);
  if (slot == invalid_order_index) {
    return {InsertStatus::CapacityExhausted};
  }

  const IndexInsertStatus insert_status = index_.insert(order.id, slot);
  if (insert_status != IndexInsertStatus::Ok) {
    // Roll back the acquired pool index before any FIFO/aggregate mutation.
    pool_.release(slot);
    return {insert_status == IndexInsertStatus::Duplicate
                ? InsertStatus::DuplicateOrderId
                : InsertStatus::CapacityExhausted};
  }

  if (order.side == Side::Bid) {
    bids_.append(pool_, slot);
  } else {
    asks_.append(pool_, slot);
  }
  return {InsertStatus::Ok};
}

std::optional<OrderView> OrderBook::best(Side side) const noexcept {
  const OrderIndex slot =
      side == Side::Bid ? bids_.best_order(pool_) : asks_.best_order(pool_);
  if (slot == invalid_order_index) {
    return std::nullopt;
  }
  return view_for(slot);
}

EraseResult OrderBook::erase(OrderId id) noexcept {
  const OrderIndex slot = index_.find(id);
  if (slot == invalid_order_index) {
    return {EraseStatus::NotFound, {}};
  }

  return {EraseStatus::Ok, remove_active_order(slot)};
}

SetRemainingResult OrderBook::set_remaining(OrderId id,
                                             Quantity new_remaining) noexcept {
  if (new_remaining == 0) {
    return {SetRemainingStatus::InvalidQuantity, {}};
  }

  const OrderIndex slot = index_.find(id);
  if (slot == invalid_order_index) {
    return {SetRemainingStatus::NotFound, {}};
  }

  detail::Order& order = pool_.get_unchecked(slot);
  const Quantity previous_remaining = order.remaining;

  if (order.side == Side::Bid) {
    bids_.set_remaining(pool_, slot, new_remaining);
  } else {
    asks_.set_remaining(pool_, slot, new_remaining);
  }
  return {SetRemainingStatus::Ok, previous_remaining};
}

bool OrderBook::validate_invariants() const noexcept {
  if (!pool_.validate_freelist() || !validate_side(Side::Bid) ||
      !validate_side(Side::Ask)) {
    return false;
  }
  if (index_.size() != active_order_count() || index_.tombstone_count() != 0) {
    return false;
  }

  std::unique_ptr<unsigned char[]> fifo_seen;
  try {
    fifo_seen = std::make_unique<unsigned char[]>(config_.max_orders);
  } catch (...) {
    return false;
  }

  const auto mark_side = [this, &fifo_seen](const auto& book) noexcept {
    for (std::size_t segment_index = 0; segment_index < book.segment_count();
         ++segment_index) {
      const PriceSegment& segment = book.segment(segment_index);
      for (std::uint32_t offset = 0; offset < prices_per_segment; ++offset) {
        const PriceLevel& level = segment.levels[offset];
        OrderCapacity steps = 0;
        for (OrderIndex current = level.head; current != invalid_order_index;
             current = pool_.get_unchecked(current).next) {
          if (!pool_.contains(current) || steps++ > config_.max_orders) {
            return false;
          }
          ++fifo_seen[current];
        }
      }
    }
    return true;
  };

  if (!mark_side(bids_) || !mark_side(asks_)) {
    return false;
  }

  OrderCapacity fifo_count = 0;
  for (OrderIndex slot = 0; slot < config_.max_orders; ++slot) {
    if (fifo_seen[slot] > 1) {
      return false;
    }
    if (fifo_seen[slot] == 1) {
      ++fifo_count;
    }
  }

  if (fifo_count != active_order_count()) {
    return false;
  }

  for (OrderIndex slot = 0; slot < config_.max_orders; ++slot) {
    if (pool_.contains(slot)) {
      if (fifo_seen[slot] != 1) {
        return false;
      }
      if (index_.find(pool_.get_unchecked(slot).id) != slot) {
        return false;
      }
    } else if (fifo_seen[slot] != 0) {
      return false;
    }
  }

  return index_.for_each([this](OrderId id, OrderIndex slot) noexcept {
    return pool_.contains(slot) && pool_.get_unchecked(slot).id == id;
  });
}

std::uint32_t OrderBook::active_order_count() const noexcept {
  return bids_.order_count() + asks_.order_count();
}

bool OrderBook::price_in_range(PriceTick price) const noexcept {
  return price >= config_.min_price_tick && price <= config_.max_price_tick;
}

OrderView OrderBook::view_for(OrderIndex slot) const noexcept {
  const detail::Order& order = pool_.get_unchecked(slot);
  return {order.id, order.owner_id, order.side, order.price, order.remaining};
}

OrderView OrderBook::remove_active_order(OrderIndex slot) noexcept {
  detail::Order& order = pool_.get_unchecked(slot);
  const OrderView removed{order.id, order.owner_id, order.side, order.price,
                          order.remaining};
  (void)index_.erase(order.id);
  if (order.side == Side::Bid) {
    bids_.remove(pool_, slot);
  } else {
    asks_.remove(pool_, slot);
  }
  pool_.release(slot);
  return removed;
}

bool OrderBook::validate_side(Side side) const noexcept {
  const auto validate = [this, side](const auto& book) noexcept {
    std::uint32_t counted_orders = 0;
    Quantity counted_quantity = 0;
    bool has_best = false;
    PriceTick expected_best{};

    for (std::size_t segment_index = 0; segment_index < book.segment_count();
         ++segment_index) {
      const PriceSegment& segment = book.segment(segment_index);
      std::uint64_t expected_mask = 0;

      for (std::uint32_t offset = 0; offset < prices_per_segment; ++offset) {
        const PriceLevel& level = segment.levels[offset];
        const bool should_be_active = level.order_count != 0;
        if (should_be_active) {
          expected_mask |= (std::uint64_t{1} << offset);
        }

        if (level.order_count == 0) {
          if (level.head != invalid_order_index ||
              level.tail != invalid_order_index || level.total_quantity != 0) {
            return false;
          }
          continue;
        }

        if (level.head == invalid_order_index ||
            level.tail == invalid_order_index ||
            !pool_.contains(level.head) || !pool_.contains(level.tail)) {
          return false;
        }
        if (pool_.get_unchecked(level.head).prev != invalid_order_index ||
            pool_.get_unchecked(level.tail).next != invalid_order_index) {
          return false;
        }

        std::uint32_t level_count = 0;
        Quantity level_quantity = 0;
        OrderIndex previous = invalid_order_index;
        for (OrderIndex current = level.head; current != invalid_order_index;
             current = pool_.get_unchecked(current).next) {
          if (!pool_.contains(current)) {
            return false;
          }
          const detail::Order& order = pool_.get_unchecked(current);
          if (order.side != side ||
              order.price != book.price_from_local(segment_index, offset) ||
              order.prev != previous) {
            return false;
          }
          previous = current;
          ++level_count;
          level_quantity += order.remaining;
          if (level_count > config_.max_orders) {
            return false;
          }
        }

        if (previous != level.tail || level_count != level.order_count ||
            level_quantity != level.total_quantity) {
          return false;
        }

        counted_orders += level_count;
        counted_quantity += level_quantity;
        const PriceTick price = book.price_from_local(segment_index, offset);
        if (!has_best ||
            (side == Side::Ask ? price < expected_best : price > expected_best)) {
          has_best = true;
          expected_best = price;
        }
      }

      if (segment.active_mask != expected_mask) {
        return false;
      }
    }

    if (counted_orders != book.order_count() ||
        counted_quantity != book.total_quantity()) {
      return false;
    }
    if (has_best != !book.empty()) {
      return false;
    }
    if (has_best && book.best_price() != expected_best) {
      return false;
    }
    return true;
  };

  return side == Side::Bid ? validate(bids_) : validate(asks_);
}

} // namespace fexma::order_book
