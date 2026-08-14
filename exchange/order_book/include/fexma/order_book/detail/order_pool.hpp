/**
 * @file order_pool.hpp
 * @brief Fixed-size intrusive storage pool for resting orders.
 */
#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <stdexcept>
#include <utility>

#include <fexma/order_book/types.hpp>

namespace fexma::order_book::detail {

struct Order {
  Order() = default;
  Order(const Order&) = default;
  Order(Order&&) noexcept = default;
  Order& operator=(const Order&) = delete;
  Order& operator=(Order&&) noexcept = delete;

  OrderId id;
  OwnerId owner_id;
  PriceTick price;
  Quantity remaining;
  OrderIndex prev;
  OrderIndex next;
  Side side;

private:
  friend class OrderPool;
  friend class OrderPoolTestAccess;
  // Kept as an O(1) pool-membership guard for release() and diagnostics.
  bool in_use;
};

// Internal implementation component.
// No API or ABI stability is guaranteed.
/**
 * @brief Fixed-capacity intrusive storage for resting orders.
 *
 * OrderPool owns one contiguous Order array and returns stable OrderIndex
 * values instead of pointers. Runtime mutation uses these indices to avoid heap
 * allocation, pointer ownership, and allocator jitter on the hot path.
 *
 * Construction validates capacity, allocates the array, and walks every slot to
 * build the freelist. OrderBook instances are expected to be constructed on the
 * final matcher/owner thread after CPU affinity and NUMA policy have already
 * been selected. The freelist build provides first-touch for the array pages,
 * but OS memory locking remains a platform/runtime responsibility; this
 * constructor is not a substitute for mlockall, VirtualLock, or an equivalent.
 *
 * Free slots are kept in a singly linked LIFO freelist headed by free_head_.
 * The freelist reuses Order::next while a slot is free. emplace() activates a
 * slot, initializes the payload, and resets FIFO links. release() returns the
 * slot to the freelist in O(1).
 *
 * get_unchecked() is the fast-path accessor for active slots. Debug builds
 * assert that the slot is in range and currently active; Release builds keep the
 * unchecked fast path. A slot index is invalid after release(). Order::in_use
 * protects invalid and double release diagnostics, but it cannot detect stale
 * indices after a slot has been released and reused. OrderIndex must stay within
 * the synchronous lifecycle controlled by OrderBook.
 *
 * @warning Not thread-safe. A single external owner must serialize access.
 * @note Invalid indices and double release are caller bugs. Debug builds assert;
 * Release builds defensively ignore them without mutating the freelist.
 */
class OrderPool final {
  friend class OrderPoolTestAccess;

public:
  explicit OrderPool(OrderCapacity capacity)
      : orders_(make_orders(validate_capacity(capacity))),
        capacity_(capacity) {
    initialize_freelist();
  }

  OrderPool(const OrderPool&) = delete;
  OrderPool& operator=(const OrderPool&) = delete;
  OrderPool(OrderPool&& other) noexcept
      : orders_(std::move(other.orders_)),
        capacity_(std::exchange(other.capacity_, 0)),
        free_head_(std::exchange(other.free_head_, invalid_order_index)),
        free_count_(std::exchange(other.free_count_, 0)) {}

  OrderPool& operator=(OrderPool&& other) noexcept {
    if (this != &other) {
      orders_ = std::move(other.orders_);
      capacity_ = std::exchange(other.capacity_, 0);
      free_head_ = std::exchange(other.free_head_, invalid_order_index);
      free_count_ = std::exchange(other.free_count_, 0);
    }
    return *this;
  }

  [[nodiscard]] OrderIndex emplace(OrderId id, OwnerId owner_id,
                                   PriceTick price, Quantity remaining,
                                   Side side) noexcept {
    const OrderIndex slot = acquire();
    if (slot == invalid_order_index) {
      return invalid_order_index;
    }

    Order& order = orders_[slot];
    order.id = id;
    order.owner_id = owner_id;
    order.price = price;
    order.remaining = remaining;
    order.prev = invalid_order_index;
    order.next = invalid_order_index;
    order.side = side;
    return slot;
  }

  void release(OrderIndex slot) noexcept {
    const bool valid = slot < capacity_ && orders_[slot].in_use;
#ifndef NDEBUG
    assert(valid);
#endif
    if (!valid) [[unlikely]] {
      return;
    }

#ifndef NDEBUG
    assert(free_count_ < capacity_);
    poison_free_slot(slot);
#else
    orders_[slot].in_use = false;
#endif
    Order& order = orders_[slot];
    order.next = free_head_;
    free_head_ = slot;
    ++free_count_;
  }

  [[nodiscard]] Order& get_unchecked(OrderIndex slot) noexcept {
    assert(slot < capacity_);
    assert(orders_[slot].in_use);
    return orders_[slot];
  }

  [[nodiscard]] const Order& get_unchecked(OrderIndex slot) const noexcept {
    assert(slot < capacity_);
    assert(orders_[slot].in_use);
    return orders_[slot];
  }

  // Diagnostic API; not hot path.
  [[nodiscard]] bool contains(OrderIndex slot) const noexcept {
    return slot < capacity_ && orders_[slot].in_use;
  }

  // Diagnostic API; not hot path.
  [[nodiscard]] bool validate_freelist() const noexcept {
    if (capacity_ == 0) {
      return free_head_ == invalid_order_index && free_count_ == 0;
    }

    std::unique_ptr<unsigned char[]> seen;
    try {
      seen = std::make_unique<unsigned char[]>(
          static_cast<std::size_t>(capacity_));
    } catch (...) {
      // Current bool diagnostics cannot distinguish scratch allocation failure
      // from an actual structural freelist error.
      return false;
    }

    OrderCapacity count = 0;
    for (OrderIndex current = free_head_; current != invalid_order_index;
         current = orders_[current].next) {
      if (current >= capacity_ || orders_[current].in_use ||
          seen[current] != 0) {
        return false;
      }
      seen[current] = 1;
      ++count;
      if (count > capacity_) {
        return false;
      }
    }
    if (count != free_count_) {
      return false;
    }

    for (OrderIndex slot = 0; slot < capacity_; ++slot) {
      if (orders_[slot].in_use) {
        if (seen[slot] != 0) {
          return false;
        }
      } else if (seen[slot] != 1) {
        return false;
      }
    }
    return true;
  }

private:
  [[nodiscard]] static std::unique_ptr<Order[]> make_orders(
      OrderCapacity capacity) {
    return capacity == 0 ? nullptr
                         : std::make_unique_for_overwrite<Order[]>(
                               static_cast<std::size_t>(capacity));
  }

  [[nodiscard]] OrderIndex acquire() noexcept {
    if (free_head_ == invalid_order_index) {
      return invalid_order_index;
    }

#ifndef NDEBUG
    assert(free_count_ > 0);
    assert(free_head_ < capacity_);
    assert(!orders_[free_head_].in_use);
#endif
    const OrderIndex slot = free_head_;
    Order& order = orders_[slot];
    free_head_ = order.next;
    order.in_use = true;
    --free_count_;
    return slot;
  }

  void initialize_freelist() noexcept {
    assert(capacity_ <= max_capacity());
    free_head_ = capacity_ == 0 ? invalid_order_index : 0;
    free_count_ = capacity_;
    for (OrderIndex slot = 0; slot < capacity_; ++slot) {
#ifndef NDEBUG
      poison_free_slot(slot);
#endif
      orders_[slot].next =
          slot + 1 == capacity_ ? invalid_order_index : slot + 1;
      orders_[slot].in_use = false;
    }
  }

  void poison_free_slot(OrderIndex slot) noexcept {
    Order& order = orders_[slot];
    order.remaining = 0;
    order.prev = invalid_order_index;
    order.next = invalid_order_index;
    order.side = Side::Bid;
    order.in_use = false;
    order.id = (std::numeric_limits<OrderId>::max)();
    order.owner_id = (std::numeric_limits<OwnerId>::max)();
    order.price = (std::numeric_limits<PriceTick>::max)();
  }

  [[nodiscard]] static constexpr OrderCapacity max_capacity() noexcept {
    constexpr auto order_index_max = (std::numeric_limits<OrderIndex>::max)();
    constexpr auto order_capacity_max =
        (std::numeric_limits<OrderCapacity>::max)();
    if constexpr (order_capacity_max < order_index_max) {
      return order_capacity_max;
    } else {
      return static_cast<OrderCapacity>(order_index_max - 1);
    }
  }

  [[nodiscard]] static OrderCapacity validate_capacity(OrderCapacity capacity) {
    if (capacity > max_capacity()) {
      throw std::invalid_argument("OrderPool capacity exceeds OrderIndex range");
    }
    return capacity;
  }

  std::unique_ptr<Order[]> orders_;
  OrderCapacity capacity_{};
  OrderIndex free_head_{invalid_order_index};
  OrderCapacity free_count_{};
};

} // namespace fexma::order_book::detail
