/**
 * @file order_pool.hpp
 * @brief Fixed-size intrusive storage pool for resting orders.
 *
 * OrderPool owns one contiguous Order array. Free slots are linked through the
 * same slot-index field used by FIFO links, so acquire/release stay O(1) and
 * do not allocate after construction.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>

#include <fexma/order_book/types.hpp>

namespace fexma::order_book {

struct Order {
  OrderId id{};
  OwnerId owner_id{};
  PriceTick price{};
  Quantity remaining{};
  OrderSlot prev{invalid_order_slot};
  OrderSlot next{invalid_order_slot};
  Side side{Side::Bid};
  bool in_use{false};
};

/**
 * @brief Fixed-capacity pool of Order objects addressed by OrderSlot.
 *
 * @warning Not thread-safe. A single external owner must serialize access.
 * @note release() ignores invalid slots and double-release attempts.
 */
class OrderPool {
public:
  OrderPool() = default;

  explicit OrderPool(OrderSlot capacity)
      : orders_(capacity == 0 ? nullptr : std::make_unique<Order[]>(capacity)),
        capacity_(capacity) {
    initialize_freelist();
  }

  OrderPool(const OrderPool&) = delete;
  OrderPool& operator=(const OrderPool&) = delete;
  OrderPool(OrderPool&&) noexcept = default;
  OrderPool& operator=(OrderPool&&) noexcept = default;

  void warm_up(std::uint32_t page_size = 4096) noexcept {
    last_warm_up_ = touch_pages(
        orders_.get(), sizeof(Order) * static_cast<std::size_t>(capacity_),
        page_size);
    initialize_freelist();
  }

  [[nodiscard]] OrderSlot acquire() noexcept {
    if (free_head_ == invalid_order_slot) {
      return invalid_order_slot;
    }

    const OrderSlot slot = free_head_;
    Order& order = orders_[slot];
    // Free slots use Order::next as freelist linkage; the acquired slot is
    // cleared before becoming visible as active storage.
    free_head_ = order.next;
    order = Order{};
    order.in_use = true;
    --free_count_;
    return slot;
  }

  void release(OrderSlot slot) noexcept {
    if (slot >= capacity_ || !orders_[slot].in_use) {
      return;
    }

    poison_free_slot(slot);
    orders_[slot].next = free_head_;
    orders_[slot].in_use = false;
    free_head_ = slot;
    ++free_count_;
  }

  [[nodiscard]] Order& operator[](OrderSlot slot) noexcept {
    return orders_[slot];
  }

  [[nodiscard]] const Order& operator[](OrderSlot slot) const noexcept {
    return orders_[slot];
  }

  [[nodiscard]] Order* data() noexcept {
    return orders_.get();
  }

  [[nodiscard]] const Order* data() const noexcept {
    return orders_.get();
  }

  [[nodiscard]] OrderSlot capacity() const noexcept {
    return capacity_;
  }

  [[nodiscard]] OrderSlot free_count() const noexcept {
    return free_count_;
  }

  [[nodiscard]] bool in_use(OrderSlot slot) const noexcept {
    return slot < capacity_ && orders_[slot].in_use;
  }

  [[nodiscard]] OrderSlot free_head() const noexcept {
    return free_head_;
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
    auto* raw = static_cast<volatile std::uint8_t*>(memory);
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
    free_head_ = capacity_ == 0 ? invalid_order_slot : 0;
    free_count_ = capacity_;
    for (OrderSlot slot = 0; slot < capacity_; ++slot) {
      poison_free_slot(slot);
      orders_[slot].next =
          slot + 1 < capacity_ ? slot + 1 : invalid_order_slot;
      orders_[slot].in_use = false;
    }
  }

  void poison_free_slot(OrderSlot slot) noexcept {
    orders_[slot] = Order{};
#ifndef NDEBUG
    orders_[slot].id = (std::numeric_limits<OrderId>::max)();
    orders_[slot].owner_id = (std::numeric_limits<OwnerId>::max)();
    orders_[slot].price = (std::numeric_limits<PriceTick>::max)();
#endif
  }

  std::unique_ptr<Order[]> orders_;
  OrderSlot capacity_{};
  OrderSlot free_head_{invalid_order_slot};
  OrderSlot free_count_{};
  WarmUpTouchStats last_warm_up_{};
};

} // namespace fexma::order_book
