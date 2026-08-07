/**
 * @file types.hpp
 * @brief Public scalar types and compact result codes for fexma::order_book.
 *
 * The file contains value-only types shared by the standalone order-book
 * storage component. It intentionally contains no matcher commands, events,
 * account state, networking, or persistence records.
 */
#pragma once

#include <cstdint>
#include <cstddef>
#include <limits>

namespace fexma::order_book {

/** @brief Opaque external order identifier used by erase/set_remaining. */
using OrderId = std::uint64_t;
/** @brief Opaque owner identifier carried through snapshots. */
using OwnerId = std::uint64_t;
/** @brief Integer price in the instrument's minimum tick. */
using PriceTick = std::uint32_t;
/** @brief Integer quantity in the instrument's minimum quantity step. */
using Quantity = std::uint32_t;
/** @brief Index into the fixed order pool; never exposed by OrderBook API. */
using OrderIndex = std::uint32_t;
/** @brief Fixed order-pool capacity/count domain. */
using OrderCapacity = std::uint32_t;

/** @brief Resting order side. */
enum class Side : std::uint8_t {
  Bid,
  Ask
};

/** @brief Sentinel index value used for FIFO links, freelist end, and selection. */
inline constexpr OrderIndex invalid_order_index =
    (std::numeric_limits<OrderIndex>::max)();

inline constexpr std::uint32_t price_segment_shift = 6;
inline constexpr std::uint32_t prices_per_segment = 64;
inline constexpr std::uint32_t price_offset_mask = prices_per_segment - 1;

/**
 * @brief Fixed-capacity configuration for one OrderBook instance.
 *
 * Construction allocates memory for the configured order pool, bid/ask price
 * segment arrays, and fixed OrderId index. Runtime mutation methods do not
 * allocate after construction and warm-up.
 */
struct OrderBookConfig {
  PriceTick min_price_tick{};
  PriceTick max_price_tick{};
  OrderCapacity max_orders{};
  std::uint32_t page_size{4096};
};

/** @brief Input value for adding one already-resting order. */
struct RestingOrderData {
  OrderId id{};
  OwnerId owner_id{};
  Side side{};
  PriceTick price{};
  Quantity quantity{};
};

/** @brief Immutable snapshot returned by best() and erase(). */
struct OrderView {
  OrderId id{};
  OwnerId owner_id{};
  Side side{};
  PriceTick price{};
  Quantity remaining{};
};

/** @brief Number of bytes/pages touched by a warm-up pass. */
struct WarmUpTouchStats {
  std::size_t bytes{};
  std::size_t pages{};
};

/** @brief Explicit status for insert(). */
enum class InsertStatus : std::uint8_t {
  Ok,
  DuplicateOrderId,
  CapacityExhausted,
  PriceOutOfRange,
  InvalidQuantity
};

/** @brief Explicit status for set_remaining(). */
enum class SetRemainingStatus : std::uint8_t {
  Ok,
  NotFound,
  InvalidQuantity
};

/** @brief Explicit status for erase(). */
enum class EraseStatus : std::uint8_t {
  Ok,
  NotFound
};

/** @brief Small result wrapper for insert(). */
struct InsertResult {
  InsertStatus status{InsertStatus::Ok};

  [[nodiscard]] bool ok() const noexcept {
    return status == InsertStatus::Ok;
  }
};

/** @brief Small result wrapper for set_remaining(). */
struct SetRemainingResult {
  SetRemainingStatus status{SetRemainingStatus::Ok};
  Quantity previous_remaining{};

  [[nodiscard]] bool ok() const noexcept {
    return status == SetRemainingStatus::Ok;
  }
};

/** @brief Small result wrapper for erase(). */
struct EraseResult {
  EraseStatus status{EraseStatus::Ok};
  OrderView removed{};

  [[nodiscard]] bool ok() const noexcept {
    return status == EraseStatus::Ok;
  }
};

} // namespace fexma::order_book
