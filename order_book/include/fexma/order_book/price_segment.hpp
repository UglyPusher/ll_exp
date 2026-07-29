#pragma once

#include <array>
#include <bit>
#include <cstdint>

#include <fexma/order_book/price_level.hpp>

namespace fexma::order_book {

struct PriceSegment {
  std::array<PriceLevel, prices_per_segment> levels{};
  std::uint64_t active_mask{};

  [[nodiscard]] bool empty() const noexcept {
    return active_mask == 0;
  }

  [[nodiscard]] std::uint32_t best_ask_offset() const noexcept {
    return static_cast<std::uint32_t>(std::countr_zero(active_mask));
  }

  [[nodiscard]] std::uint32_t best_bid_offset() const noexcept {
    return 63U - static_cast<std::uint32_t>(std::countl_zero(active_mask));
  }
};

} // namespace fexma::order_book
