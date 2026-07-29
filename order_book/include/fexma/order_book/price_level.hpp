#pragma once

#include <cstdint>

#include <fexma/order_book/types.hpp>

namespace fexma::order_book {

struct PriceLevel {
  OrderSlot head{invalid_order_slot};
  OrderSlot tail{invalid_order_slot};
  Quantity total_quantity{};
  std::uint32_t order_count{};

  [[nodiscard]] bool empty() const noexcept {
    return order_count == 0;
  }
};

} // namespace fexma::order_book
