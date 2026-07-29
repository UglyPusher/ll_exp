/**
 * @file price_segment.hpp
 * @brief 64-price segment and active-level bit mask.
 *
 * A segment owns 64 PriceLevel objects. active_mask mirrors non-empty levels
 * and lets SideBook find the best offset with C++20 bit operations.
 */
#pragma once

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>

#include <fexma/order_book/price_level.hpp>

namespace fexma::order_book {

/** @brief Fixed block of 64 price levels plus occupancy mask. */
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

  [[nodiscard]] static constexpr std::size_t segment_size() noexcept {
    return sizeof(PriceSegment);
  }

  [[nodiscard]] static constexpr std::size_t segment_align() noexcept {
    return alignof(PriceSegment);
  }
};

} // namespace fexma::order_book
