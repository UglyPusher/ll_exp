/**
 * @file order_book.cpp
 * @brief OrderBook facade implementation and slow invariant validation.
 *
 * The implementation coordinates the fixed pool, fixed OrderId index, and the
 * bid/ask side books. It keeps structural operations local and deliberately
 * avoids matcher policy, event generation, WAL, callbacks, locks, and atomics.
 */
#include <fexma/order_book/order_book.hpp>

#include <cassert>
#include <memory>

namespace fexma::order_book {

OrderBook::OrderBook(const OrderBookConfig& config)
    : config_(config),
      pool_(config.max_orders),
      index_(config.max_orders),
      bids_(config.min_price_tick, config.max_price_tick),
      asks_(config.min_price_tick, config.max_price_tick) {}

void OrderBook::warm_up() noexcept {
  assert(empty());
  pool_.warm_up(config_.page_size);
  index_.warm_up(config_.page_size);
  bids_.warm_up(config_.page_size);
  asks_.warm_up(config_.page_size);
  selected_order_ = invalid_order_slot;
  selected_generation_ = generation_;
}

PutResult OrderBook::put(const RestingOrderData& order) noexcept {
  // Failure checks happen before any mutation so rejected puts preserve the
  // logical book, including the selected-order protocol state.
  if (order.quantity == 0) {
    return {PutStatus::InvalidQuantity};
  }
  if (!price_in_range(order.price)) {
    return {PutStatus::PriceOutOfRange};
  }
  if (index_.find(order.id) != invalid_order_slot) {
    return {PutStatus::DuplicateOrderId};
  }

  const OrderSlot slot = pool_.acquire();
  if (slot == invalid_order_slot) {
    return {PutStatus::PoolExhausted};
  }

  Order& stored = pool_[slot];
  stored.id = order.id;
  stored.owner_id = order.owner_id;
  stored.price = order.price;
  stored.remaining = order.quantity;
  stored.side = order.side;

  const IndexInsertStatus insert_status = index_.insert(order.id, slot);
  if (insert_status != IndexInsertStatus::Ok) {
    // Roll back the acquired pool slot before any FIFO/aggregate mutation.
    pool_.release(slot);
    return {insert_status == IndexInsertStatus::Duplicate
                ? PutStatus::DuplicateOrderId
                : PutStatus::IndexFull};
  }

  if (order.side == Side::Bid) {
    bids_.append(pool_, slot);
  } else {
    asks_.append(pool_, slot);
  }
  // Selection is invalidated only after the FIFO/index/pool mutation commits.
  mark_mutation();
  return {PutStatus::Ok};
}

std::optional<BestOrderView>
OrderBook::select_best_opposite(Side incoming_side) noexcept {
  const OrderSlot slot = incoming_side == Side::Bid ? asks_.best_order(pool_)
                                                    : bids_.best_order(pool_);
  selected_order_ = slot;
  if (slot == invalid_order_slot) {
    selected_generation_ = generation_;
    return std::nullopt;
  }

  const Order& order = pool_[slot];
  selected_generation_ = generation_;
  return BestOrderView{order.id, order.owner_id, order.price, order.remaining};
}

void OrderBook::decrement_selected(Quantity quantity) noexcept {
  assert(selected_order_ != invalid_order_slot);
  assert(selected_generation_ == generation_);
  assert(quantity > 0);

  const OrderSlot slot = selected_order_;
  selected_order_ = invalid_order_slot;
  Order& order = pool_[slot];
  assert(order.in_use);
  assert(quantity <= order.remaining);

  // Release builds trust the selected slot and avoid an index lookup. Debug
  // assertions guard stale selection through the generation check above.
  if (order.side == Side::Bid) {
    bids_.reduce(pool_, slot, quantity);
  } else {
    asks_.reduce(pool_, slot, quantity);
  }

  if (order.remaining == 0) {
    (void)index_.erase(order.id);
    if (order.side == Side::Bid) {
      bids_.remove(pool_, slot);
    } else {
      asks_.remove(pool_, slot);
    }
    pool_.release(slot);
  }
  ++generation_;
  selected_generation_ = generation_;
}

CancelResult OrderBook::cancel(OrderId id) noexcept {
  const OrderSlot slot = index_.find(id);
  if (slot == invalid_order_slot) {
    return {CancelStatus::NotFound};
  }

  // NotFound is non-mutating. A successful cancel invalidates any selection
  // before unlinking so stale-slot access cannot survive the call.
  mark_mutation();
  remove_active_order(slot);
  return {CancelStatus::Ok};
}

ChangeResult OrderBook::change(OrderId id, const OrderChange& change) noexcept {
  const OrderSlot slot = index_.find(id);
  if (slot == invalid_order_slot) {
    return {ChangeStatus::NotFound};
  }

  Order& order = pool_[slot];
  if (change.new_remaining >= order.remaining) {
    return {ChangeStatus::InvalidQuantity};
  }
  // Invalid reductions are non-mutating; successful reductions invalidate
  // selection because the cached snapshot may no longer match the slot.
  mark_mutation();
  if (change.new_remaining == 0) {
    remove_active_order(slot);
    return {ChangeStatus::Ok};
  }

  const Quantity delta = order.remaining - change.new_remaining;
  if (order.side == Side::Bid) {
    bids_.reduce(pool_, slot, delta);
  } else {
    asks_.reduce(pool_, slot, delta);
  }
  return {ChangeStatus::Ok};
}

bool OrderBook::validate_invariants() const noexcept {
  if (selected_order_ != invalid_order_slot &&
      (selected_order_ >= pool_.capacity() || !pool_.in_use(selected_order_) ||
       selected_generation_ != generation_)) {
    return false;
  }

  if (!validate_side(Side::Bid) || !validate_side(Side::Ask)) {
    return false;
  }

  std::unique_ptr<unsigned char[]> fifo_seen;
  std::unique_ptr<unsigned char[]> free_seen;
  try {
    fifo_seen = std::make_unique<unsigned char[]>(pool_.capacity());
    free_seen = std::make_unique<unsigned char[]>(pool_.capacity());
  } catch (...) {
    return false;
  }

  const auto mark_side = [this, &fifo_seen](const auto& book) noexcept {
    for (std::size_t segment_index = 0; segment_index < book.segment_count();
         ++segment_index) {
      const PriceSegment& segment = book.segment(segment_index);
      for (std::uint32_t offset = 0; offset < prices_per_segment; ++offset) {
        const PriceLevel& level = segment.levels[offset];
        OrderSlot steps = 0;
        for (OrderSlot current = level.head; current != invalid_order_slot;
             current = pool_[current].next) {
          if (current >= pool_.capacity() || steps++ > pool_.capacity()) {
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

  OrderSlot fifo_count = 0;
  for (OrderSlot slot = 0; slot < pool_.capacity(); ++slot) {
    if (fifo_seen[slot] > 1) {
      return false;
    }
    if (fifo_seen[slot] == 1) {
      ++fifo_count;
    }
  }

  OrderSlot free_count = 0;
  for (OrderSlot slot = pool_.free_head(); slot != invalid_order_slot;
       slot = pool_[slot].next) {
    if (slot >= pool_.capacity() || free_seen[slot] != 0) {
      return false;
    }
    free_seen[slot] = 1;
    ++free_count;
  }

  if (free_count != pool_.free_count()) {
    return false;
  }
  if (fifo_count + free_count != pool_.capacity()) {
    return false;
  }

  for (OrderSlot slot = 0; slot < pool_.capacity(); ++slot) {
    if (pool_.in_use(slot)) {
      if (fifo_seen[slot] != 1 || free_seen[slot] != 0) {
        return false;
      }
      if (index_.find(pool_[slot].id) != slot) {
        return false;
      }
    } else if (fifo_seen[slot] != 0 || free_seen[slot] != 1 ||
               index_.find(pool_[slot].id) != invalid_order_slot) {
      return false;
    }
  }

  return index_.for_each([this](OrderId id, OrderSlot slot) noexcept {
    return slot < pool_.capacity() && pool_.in_use(slot) && pool_[slot].id == id;
  });
}

std::optional<PriceTick> OrderBook::best_bid() const noexcept {
  if (bids_.empty()) {
    return std::nullopt;
  }
  return bids_.best_price();
}

std::optional<PriceTick> OrderBook::best_ask() const noexcept {
  if (asks_.empty()) {
    return std::nullopt;
  }
  return asks_.best_price();
}

bool OrderBook::price_in_range(PriceTick price) const noexcept {
  return price >= config_.min_price_tick && price <= config_.max_price_tick;
}

void OrderBook::invalidate_selection() noexcept {
  selected_order_ = invalid_order_slot;
  selected_generation_ = generation_;
}

void OrderBook::mark_mutation() noexcept {
  ++generation_;
  // Generation moves before clearing selection so validate_invariants() can
  // detect any stale selected slot accidentally left behind by future edits.
  invalidate_selection();
}

void OrderBook::remove_active_order(OrderSlot slot) noexcept {
  Order& order = pool_[slot];
  (void)index_.erase(order.id);
  if (order.side == Side::Bid) {
    bids_.remove(pool_, slot);
  } else {
    asks_.remove(pool_, slot);
  }
  pool_.release(slot);
}

bool OrderBook::slot_in_any_fifo(OrderSlot slot) const noexcept {
  if (slot >= pool_.capacity()) {
    return false;
  }

  const auto scan_side = [this, slot](const auto& book) noexcept {
    for (std::size_t segment_index = 0; segment_index < book.segment_count();
         ++segment_index) {
      const PriceSegment& segment = book.segment(segment_index);
      for (std::uint32_t offset = 0; offset < prices_per_segment; ++offset) {
        OrderSlot current = segment.levels[offset].head;
        OrderSlot steps = 0;
        while (current != invalid_order_slot) {
          if (current >= pool_.capacity() || steps++ > pool_.capacity()) {
            return false;
          }
          if (current == slot) {
            return true;
          }
          current = pool_[current].next;
        }
      }
    }
    return false;
  };

  return scan_side(bids_) || scan_side(asks_);
}

bool OrderBook::slot_in_freelist(OrderSlot slot) const noexcept {
  for (OrderSlot current = pool_.free_head(); current != invalid_order_slot;
       current = pool_[current].next) {
    if (current == slot) {
      return true;
    }
  }
  return false;
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
          if (level.head != invalid_order_slot ||
              level.tail != invalid_order_slot || level.total_quantity != 0) {
            return false;
          }
          continue;
        }

        if (level.head == invalid_order_slot ||
            level.tail == invalid_order_slot ||
            level.head >= pool_.capacity() || level.tail >= pool_.capacity()) {
          return false;
        }
        if (pool_[level.head].prev != invalid_order_slot ||
            pool_[level.tail].next != invalid_order_slot) {
          return false;
        }

        std::uint32_t level_count = 0;
        Quantity level_quantity = 0;
        OrderSlot previous = invalid_order_slot;
        for (OrderSlot current = level.head; current != invalid_order_slot;
             current = pool_[current].next) {
          if (current >= pool_.capacity() || !pool_.in_use(current)) {
            return false;
          }
          const Order& order = pool_[current];
          if (order.side != side ||
              order.price != book.price_from_local(segment_index, offset) ||
              order.prev != previous) {
            return false;
          }
          previous = current;
          ++level_count;
          level_quantity += order.remaining;
          if (level_count > pool_.capacity()) {
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
