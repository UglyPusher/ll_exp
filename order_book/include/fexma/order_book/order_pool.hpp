/**
 * @file order_pool.hpp
 * @brief Fixed-size intrusive storage pool for resting orders.
 *
 * OrderPool owns one contiguous Order array. Free entries are linked through
 * the same pool-index field used by FIFO links, so acquire/release stay O(1)
 * and do not allocate after construction.
 */
#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <utility>

#include <fexma/order_book/types.hpp>

namespace fexma::order_book {

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
  // Kept as an O(1) pool-membership guard for release() and invariant checks.
  bool in_use;
};

/**
 * @brief Fixed-capacity intrusive storage for resting orders.
 *
 * OrderPool is the order book's allocator and backing store for Order objects.
 * It owns one contiguous array allocated at construction time and returns
 * stable OrderIndex values instead of pointers. Runtime mutation uses these
 * indices to avoid heap allocation, pointer ownership, and allocator jitter on
 * the hot path.
 *
 * Free slots are kept in a singly linked freelist headed by free_head_. The
 * freelist reuses Order::next as its link field while a slot is not in use.
 * Once emplace() activates a slot, Order::prev and Order::next are reset and
 * become intrusive FIFO links owned by the corresponding price level. release()
 * moves the slot back to the freelist in O(1).
 *
 * The public allocation API is intentionally narrow: emplace() is the only
 * normal way to acquire an active order, so every returned slot has a fully
 * initialized payload and clean FIFO links. operator[] is an unchecked fast-path
 * accessor in release builds; callers must pass a valid slot obtained from this
 * pool.
 *
 * @warning Not thread-safe. A single external owner must serialize access.
 * @note release() ignores invalid indices and double-release attempts.
 * @note validate_freelist() is diagnostic only and allocates scratch memory.
 */
class OrderPool {
public:
  struct Uninitialized {};

  OrderPool() = default;

  explicit OrderPool(OrderCapacity capacity)
      : OrderPool(capacity, Uninitialized{}) {
    reset();
  }

  OrderPool(OrderCapacity capacity, Uninitialized)
      : orders_(capacity == 0 ? nullptr
                              : std::make_unique_for_overwrite<Order[]>(
                                    static_cast<std::size_t>(capacity))),
        capacity_(capacity) {
    assert(capacity != invalid_order_index);
  }

  OrderPool(const OrderPool&) = delete;
  OrderPool& operator=(const OrderPool&) = delete;
  OrderPool(OrderPool&& other) noexcept
      : orders_(std::move(other.orders_)),
        capacity_(std::exchange(other.capacity_, 0)),
        free_head_(std::exchange(other.free_head_, invalid_order_index)),
        free_count_(std::exchange(other.free_count_, 0)),
        last_warm_up_(std::exchange(other.last_warm_up_, WarmUpTouchStats{})),
        initialized_(std::exchange(other.initialized_, false)) {}

  OrderPool& operator=(OrderPool&& other) noexcept {
    if (this != &other) {
      orders_ = std::move(other.orders_);
      capacity_ = std::exchange(other.capacity_, 0);
      free_head_ = std::exchange(other.free_head_, invalid_order_index);
      free_count_ = std::exchange(other.free_count_, 0);
      last_warm_up_ = std::exchange(other.last_warm_up_, WarmUpTouchStats{});
      initialized_ = std::exchange(other.initialized_, false);
    }
    return *this;
  }

  void reset() noexcept {
    initialize_freelist();
  }

  /**
   * @brief Touches backing pages only; it does not initialize Order fields.
   */
  void prefault_pages(std::uint32_t touch_stride = 4096) noexcept {
    last_warm_up_ = touch_pages(
        orders_.get(), sizeof(Order) * static_cast<std::size_t>(capacity_),
        touch_stride);
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
#ifndef NDEBUG
    assert(slot < capacity_);
    assert(orders_[slot].in_use);
#endif
    if (slot >= capacity_ || !orders_[slot].in_use) {
      return;
    }

#ifndef NDEBUG
    poison_free_slot(slot);
#endif
    Order& order = orders_[slot];
    order.next = free_head_;
    order.in_use = false;
    free_head_ = slot;
    ++free_count_;
  }

  [[nodiscard]] Order& operator[](OrderIndex slot) noexcept {
    assert(slot < capacity_);
    return orders_[slot];
  }

  [[nodiscard]] const Order& operator[](OrderIndex slot) const noexcept {
    assert(slot < capacity_);
    return orders_[slot];
  }

  [[nodiscard]] Order* data() noexcept {
    return orders_.get();
  }

  [[nodiscard]] const Order* data() const noexcept {
    return orders_.get();
  }

  [[nodiscard]] OrderCapacity capacity() const noexcept {
    return capacity_;
  }

  [[nodiscard]] OrderCapacity free_count() const noexcept {
    return free_count_;
  }

  [[nodiscard]] bool in_use(OrderIndex slot) const noexcept {
    return slot < capacity_ && orders_[slot].in_use;
  }

  [[nodiscard]] OrderIndex free_head() const noexcept {
    return free_head_;
  }

  /**
   * @brief Slow diagnostic check; allocates scratch memory and is not hot path.
   */
  [[nodiscard]] bool validate_freelist() const noexcept {
    if (!initialized_) {
      return free_head_ == invalid_order_index && free_count_ == 0;
    }

    std::unique_ptr<unsigned char[]> seen;
    try {
      seen = std::make_unique<unsigned char[]>(
          static_cast<std::size_t>(capacity_));
    } catch (...) {
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

  [[nodiscard]] WarmUpTouchStats last_warm_up_stats() const noexcept {
    return last_warm_up_;
  }

#ifdef FEXMA_ORDER_POOL_ENABLE_TEST_ACCESS
  [[nodiscard]] OrderIndex acquire_for_test() noexcept {
    return acquire();
  }

  void set_in_use_for_test(OrderIndex slot, bool value) noexcept {
    orders_[slot].in_use = value;
  }
#endif

  [[nodiscard]] static constexpr std::size_t order_size() noexcept {
    return sizeof(Order);
  }

  [[nodiscard]] static constexpr std::size_t order_align() noexcept {
    return alignof(Order);
  }

private:
  [[nodiscard]] OrderIndex acquire() noexcept {
    if (free_head_ == invalid_order_index) {
      return invalid_order_index;
    }

    const OrderIndex slot = free_head_;
    Order& order = orders_[slot];
    // Free entries use Order::next as freelist linkage. Keep acquire()
    // private so every public allocation path initializes the active payload.
    free_head_ = order.next;
    order.in_use = true;
    --free_count_;
    return slot;
  }

  static WarmUpTouchStats touch_pages(void* memory, std::size_t bytes,
                                      std::uint32_t touch_stride) noexcept {
    if (memory == nullptr || bytes == 0) {
      return {};
    }

    const std::size_t step = touch_stride == 0 ? 4096U : touch_stride;
    const std::uintptr_t start = reinterpret_cast<std::uintptr_t>(memory);
    const std::size_t first_page_offset = start % step;
    const std::size_t page_count =
        (first_page_offset + bytes + step - 1) / step;
    auto* raw = static_cast<volatile unsigned char*>(memory);
    for (std::size_t offset = 0; offset < bytes; offset += step) {
      // Volatile read/write prevents the compiler from discarding the page
      // touch while keeping OS locking and affinity outside this component.
      raw[offset] = raw[offset];
    }
    raw[bytes - 1] = raw[bytes - 1];
    return {bytes, page_count};
  }

  void initialize_freelist() noexcept {
    // invalid_order_index is the all-ones sentinel, so it must never be a real
    // addressable slot.
    assert(capacity_ != invalid_order_index);
    free_head_ = capacity_ == 0 ? invalid_order_index : 0;
    free_count_ = capacity_;
    initialized_ = true;
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
    order.id = 0;
    order.owner_id = 0;
    order.price = 0;
    order.remaining = 0;
    order.prev = invalid_order_index;
    order.next = invalid_order_index;
    order.side = Side::Bid;
    order.in_use = false;
#ifndef NDEBUG
    order.id = (std::numeric_limits<OrderId>::max)();
    order.owner_id = (std::numeric_limits<OwnerId>::max)();
    order.price = (std::numeric_limits<PriceTick>::max)();
#endif
  }

  std::unique_ptr<Order[]> orders_;
  OrderCapacity capacity_{};
  OrderIndex free_head_{invalid_order_index};
  OrderCapacity free_count_{};
  WarmUpTouchStats last_warm_up_{};
  bool initialized_{};
};

} // namespace fexma::order_book
