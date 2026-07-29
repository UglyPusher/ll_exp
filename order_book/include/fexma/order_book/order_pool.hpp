#pragma once

#include <cstddef>
#include <cstdint>
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
    touch_pages(orders_.get(), sizeof(Order) * static_cast<std::size_t>(capacity_),
                page_size);
    initialize_freelist();
  }

  [[nodiscard]] OrderSlot acquire() noexcept {
    if (free_head_ == invalid_order_slot) {
      return invalid_order_slot;
    }

    const OrderSlot slot = free_head_;
    Order& order = orders_[slot];
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

    orders_[slot] = Order{};
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

private:
  static void touch_pages(void* memory, std::size_t bytes,
                          std::uint32_t page_size) noexcept {
    if (memory == nullptr || bytes == 0) {
      return;
    }

    const std::size_t step = page_size == 0 ? 4096U : page_size;
    auto* raw = static_cast<volatile std::uint8_t*>(memory);
    for (std::size_t offset = 0; offset < bytes; offset += step) {
      raw[offset] = raw[offset];
    }
    raw[bytes - 1] = raw[bytes - 1];
  }

  void initialize_freelist() noexcept {
    free_head_ = capacity_ == 0 ? invalid_order_slot : 0;
    free_count_ = capacity_;
    for (OrderSlot slot = 0; slot < capacity_; ++slot) {
      orders_[slot] = Order{};
      orders_[slot].next =
          slot + 1 < capacity_ ? slot + 1 : invalid_order_slot;
      orders_[slot].in_use = false;
    }
  }

  std::unique_ptr<Order[]> orders_;
  OrderSlot capacity_{};
  OrderSlot free_head_{invalid_order_slot};
  OrderSlot free_count_{};
};

} // namespace fexma::order_book
