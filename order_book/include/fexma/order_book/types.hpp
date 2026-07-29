#pragma once

#include <cstdint>
#include <limits>

namespace fexma::order_book {

using OrderId = std::uint64_t;
using OwnerId = std::uint64_t;
using PriceTick = std::uint32_t;
using Quantity = std::uint32_t;
using OrderSlot = std::uint32_t;

enum class Side : std::uint8_t {
  Bid,
  Ask
};

inline constexpr OrderSlot invalid_order_slot =
    (std::numeric_limits<OrderSlot>::max)();

inline constexpr std::uint32_t price_segment_shift = 6;
inline constexpr std::uint32_t prices_per_segment = 64;
inline constexpr std::uint32_t price_offset_mask = prices_per_segment - 1;

struct OrderBookConfig {
  PriceTick min_price_tick{};
  PriceTick max_price_tick{};
  OrderSlot max_orders{};
  std::uint32_t page_size{4096};
};

struct RestingOrderData {
  OrderId id{};
  OwnerId owner_id{};
  Side side{};
  PriceTick price{};
  Quantity quantity{};
};

struct BestOrderView {
  OrderId id{};
  OwnerId owner_id{};
  PriceTick price{};
  Quantity remaining{};
};

struct OrderChange {
  Quantity new_remaining{};
};

enum class PutStatus : std::uint8_t {
  Ok,
  DuplicateOrderId,
  PoolExhausted,
  IndexFull,
  PriceOutOfRange,
  InvalidQuantity
};

enum class CancelStatus : std::uint8_t {
  Ok,
  NotFound
};

enum class ChangeStatus : std::uint8_t {
  Ok,
  NotFound,
  InvalidQuantity
};

struct PutResult {
  PutStatus status{PutStatus::Ok};

  [[nodiscard]] bool ok() const noexcept {
    return status == PutStatus::Ok;
  }
};

struct CancelResult {
  CancelStatus status{CancelStatus::Ok};

  [[nodiscard]] bool ok() const noexcept {
    return status == CancelStatus::Ok;
  }
};

struct ChangeResult {
  ChangeStatus status{ChangeStatus::Ok};

  [[nodiscard]] bool ok() const noexcept {
    return status == ChangeStatus::Ok;
  }
};

} // namespace fexma::order_book
