/**
 * @file order_pool_test_access.hpp
 * @brief Test-only accessors for OrderPool internals.
 */
#pragma once

#include <fexma/order_book/detail/order_pool.hpp>

namespace fexma::order_book::detail {

class OrderPoolTestAccess {
public:
  [[nodiscard]] static OrderCapacity capacity(const OrderPool& pool) noexcept {
    return pool.capacity_;
  }

  [[nodiscard]] static OrderCapacity free_count(
      const OrderPool& pool) noexcept {
    return pool.free_count_;
  }

  [[nodiscard]] static OrderIndex free_head(const OrderPool& pool) noexcept {
    return pool.free_head_;
  }

  [[nodiscard]] static OrderIndex next(const OrderPool& pool,
                                       OrderIndex slot) noexcept {
    return pool.orders_[slot].next;
  }

  static void set_in_use(OrderPool& pool, OrderIndex slot,
                         bool value) noexcept {
    pool.orders_[slot].in_use = value;
  }

  static void set_next(OrderPool& pool, OrderIndex slot,
                       OrderIndex next) noexcept {
    pool.orders_[slot].next = next;
  }

  static void set_free_count(OrderPool& pool,
                             OrderCapacity free_count) noexcept {
    pool.free_count_ = free_count;
  }
};

} // namespace fexma::order_book::detail
