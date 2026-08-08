/**
 * @file order_id_index_test_access.hpp
 * @brief Test-only accessors for OrderIdIndex internals.
 */
#pragma once

#include <fexma/order_book/order_id_index.hpp>

namespace fexma::order_book {

class OrderIdIndexTestAccess {
public:
  [[nodiscard]] static std::size_t
  home_bucket(const OrderIdIndex& index, OrderId id) noexcept {
    return OrderIdIndex::hash(id) & (index.bucket_count_ - 1U);
  }
};

} // namespace fexma::order_book
