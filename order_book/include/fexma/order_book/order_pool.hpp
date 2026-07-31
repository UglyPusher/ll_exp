/**
 * @file order_pool.hpp
 * @brief Fixed-size intrusive storage pool for resting orders.
 *
 * OrderPool owns one contiguous Order array. Free entries are linked through
 * the same pool-index field used by FIFO links, so acquire/release stay O(1)
 * and do not allocate after construction.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>

#include <fexma/order_book/types.hpp>

namespace fexma::order_book {

struct Order {
  OrderId id;
  OwnerId owner_id;
  PriceTick price;
  Quantity remaining;
  OrderIndex prev;
  OrderIndex next;
  Side side;
  // Kept as an O(1) pool-membership guard for release() and invariant checks.
  bool in_use;
};

/**
 * @brief Fixed-capacity pool of Order objects addressed by OrderIndex.
 *
 * @warning Not thread-safe. A single external owner must serialize access.
 * @note release() ignores invalid indices and double-release attempts.
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
        capacity_(capacity) {}

  OrderPool(const OrderPool&) = delete;
  OrderPool& operator=(const OrderPool&) = delete;
  OrderPool(OrderPool&&) noexcept = default;
  OrderPool& operator=(OrderPool&&) noexcept = default;

  void reset() noexcept {
    initialize_freelist();
  }

  void prefault_pages(std::uint32_t page_size = 4096) noexcept {
    last_warm_up_ = touch_pages(
        orders_.get(), sizeof(Order) * static_cast<std::size_t>(capacity_),
        page_size);
  }

  [[nodiscard]] OrderIndex acquire() noexcept {
    if (free_head_ == invalid_order_index) {
      return invalid_order_index;
    }

    const OrderIndex slot = free_head_;
    Order& order = orders_[slot];
    // Free entries use Order::next as freelist linkage. The caller is
    // responsible for fully initializing the active order payload.
    free_head_ = order.next;
    order.in_use = true;
    --free_count_;
    return slot;
  }

  void release(OrderIndex slot) noexcept {
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
    return orders_[slot];
  }

  [[nodiscard]] const Order& operator[](OrderIndex slot) const noexcept {
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

  [[nodiscard]] bool validate_freelist() const noexcept {
    OrderCapacity count = 0;
    for (OrderIndex current = free_head_; current != invalid_order_index;
         current = orders_[current].next) {
      if (current >= capacity_ || orders_[current].in_use) {
        return false;
      }
      ++count;
      if (count > free_count_ || count > capacity_) {
        return false;
      }
    }
    return count == free_count_;
  }

  [[nodiscard]] WarmUpTouchStats last_warm_up_stats() const noexcept {
    return last_warm_up_;
  }

  [[nodiscard]] static constexpr std::size_t order_size() noexcept {
    return sizeof(Order);
  }

  [[nodiscard]] static constexpr std::size_t order_align() noexcept {
    return alignof(Order);
  }

private:
  static WarmUpTouchStats touch_pages(void* memory, std::size_t bytes,
                                      std::uint32_t page_size) noexcept {
    if (memory == nullptr || bytes == 0) {
      return {};
    }

    const std::size_t step = page_size == 0 ? 4096U : page_size;
    auto* raw = static_cast<volatile unsigned char*>(memory);
    std::size_t pages = 0;
    for (std::size_t offset = 0; offset < bytes; offset += step) {
      // Volatile read/write prevents the compiler from discarding the page
      // touch while keeping OS locking and affinity outside this component.
      raw[offset] = raw[offset];
      ++pages;
    }
    raw[bytes - 1] = raw[bytes - 1];
    return {bytes, pages};
  }

  void initialize_freelist() noexcept {
    free_head_ = capacity_ == 0 ? invalid_order_index : 0;
    free_count_ = capacity_;
    for (OrderIndex slot = 0; slot < capacity_; ++slot) {
#ifndef NDEBUG
      poison_free_slot(slot);
#endif
      orders_[slot].next =
          slot + 1 < capacity_ ? slot + 1 : invalid_order_index;
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
};

} // namespace fexma::order_book
