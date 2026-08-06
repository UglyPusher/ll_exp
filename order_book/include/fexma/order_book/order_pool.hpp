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
#include <stdexcept>
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
 * Until reset() initializes the freelist, only capacity(), data(),
 * prefault_pages(), last_warm_up_stats(), initialized(), in_use(), and reset()
 * are valid operations. prefault_pages() only touches bytes and must not read
 * Order fields.
 *
 * Once emplace() activates a slot, Order::prev and Order::next are reset and
 * become intrusive FIFO links owned by the corresponding price level. release()
 * moves the slot back to the freelist in O(1).
 *
 * The public allocation API is intentionally narrow: emplace() is the only
 * normal way to acquire an active order, so every returned slot has a fully
 * initialized payload and clean FIFO links. operator[] is an unchecked fast-path
 * accessor in release builds; callers must pass an active slot obtained from
 * this pool. reset() invalidates every previously returned OrderIndex.
 *
 * Order::in_use protects release() and diagnostics from invalid and double
 * release, but it cannot detect a stale OrderIndex after the slot has been
 * released and reused. OrderIndex is safe only within the lifecycle established
 * by the owning OrderBook.
 *
 * @warning Not thread-safe. A single external owner must serialize access.
 * @note Invalid indices and double release are caller bugs. Debug builds assert;
 * Release builds defensively ignore them without mutating the freelist.
 * @note validate_freelist() is diagnostic only and allocates scratch memory.
 */
class OrderPool {
  struct CheckedCapacity {};

public:
  struct Uninitialized {};

  OrderPool() = default;

  explicit OrderPool(OrderCapacity capacity)
      : OrderPool(capacity, Uninitialized{}) {
    reset();
  }

  OrderPool(OrderCapacity capacity, Uninitialized)
      : OrderPool(validate_capacity(capacity), Uninitialized{},
                  CheckedCapacity{}) {}

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
   *
   * This is a best-effort first-touch helper. It does not keep pages resident
   * and is not a substitute for mlockall, VirtualLock, or a platform equivalent.
   * touch_stride must be no larger than the real page size to guarantee that
   * every page is touched. Reported page_count is based on the supplied stride,
   * not necessarily on the operating system's page size.
   */
  void prefault_pages(std::uint32_t touch_stride = 4096) noexcept {
    last_warm_up_ = touch_pages(
        orders_.get(), sizeof(Order) * static_cast<std::size_t>(capacity_),
        touch_stride);
  }

  [[nodiscard]] OrderIndex emplace(OrderId id, OwnerId owner_id,
                                   PriceTick price, Quantity remaining,
                                   Side side) noexcept {
#ifndef NDEBUG
    assert(initialized_);
#endif
    if (!initialized_) {
      return invalid_order_index;
    }

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
    const bool valid =
        initialized_ && slot < capacity_ && orders_[slot].in_use;
#ifndef NDEBUG
    assert(valid);
#endif
    if (!valid) [[unlikely]] {
      return;
    }

#ifndef NDEBUG
    assert(free_count_ < capacity_);
#endif
#ifndef NDEBUG
    poison_free_slot(slot);
#else
    orders_[slot].in_use = false;
#endif
    Order& order = orders_[slot];
    order.next = free_head_;
    free_head_ = slot;
    ++free_count_;
  }

  [[nodiscard]] Order& operator[](OrderIndex slot) noexcept {
    assert(initialized_);
    assert(slot < capacity_);
    assert(orders_[slot].in_use);
    return orders_[slot];
  }

  [[nodiscard]] const Order& operator[](OrderIndex slot) const noexcept {
    assert(initialized_);
    assert(slot < capacity_);
    assert(orders_[slot].in_use);
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

  [[nodiscard]] bool initialized() const noexcept {
    return initialized_;
  }

  [[nodiscard]] bool in_use(OrderIndex slot) const noexcept {
    return initialized_ && slot < capacity_ && orders_[slot].in_use;
  }

  [[nodiscard]] OrderIndex free_head() const noexcept {
    return free_head_;
  }

  /**
   * @brief Slow diagnostic check; allocates scratch memory and is not hot path.
   */
  [[nodiscard]] bool validate_freelist() const noexcept {
    if (!initialized_) {
      return false;
    }
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

  void set_next_for_test(OrderIndex slot, OrderIndex next) noexcept {
    orders_[slot].next = next;
  }

  [[nodiscard]] OrderIndex next_for_test(OrderIndex slot) const noexcept {
    return orders_[slot].next;
  }
#endif

  [[nodiscard]] static constexpr std::size_t order_size() noexcept {
    return sizeof(Order);
  }

  [[nodiscard]] static constexpr std::size_t order_align() noexcept {
    return alignof(Order);
  }

private:
  OrderPool(OrderCapacity capacity, Uninitialized, CheckedCapacity)
      : orders_(capacity == 0 ? nullptr
                              : std::make_unique_for_overwrite<Order[]>(
                                    static_cast<std::size_t>(capacity))),
        capacity_(capacity) {}

  [[nodiscard]] OrderIndex acquire() noexcept {
    if (free_head_ == invalid_order_index) {
      return invalid_order_index;
    }

#ifndef NDEBUG
    assert(initialized_);
    assert(free_count_ > 0);
    assert(free_head_ < capacity_);
    assert(!orders_[free_head_].in_use);
#endif
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
    assert(capacity_ <= max_capacity());
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
  WarmUpTouchStats last_warm_up_{};
  bool initialized_{};
};

} // namespace fexma::order_book
