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

/** @brief Opaque external order identifier used by cancel/change. */
using OrderId = std::uint64_t;
/** @brief Opaque owner identifier carried through snapshots. */
using OwnerId = std::uint64_t;
/** @brief Integer price in the instrument's minimum tick. */
using PriceTick = std::uint32_t;
/** @brief Integer quantity in the instrument's minimum quantity step. */
using Quantity = std::uint32_t;
/** @brief Index into the fixed order pool; never exposed by OrderBook API. */
using OrderSlot = std::uint32_t;

/** @brief Resting order side. */
enum class Side : std::uint8_t {
  Bid,
  Ask
};

/** @brief Sentinel slot value used for FIFO links, freelist end, and selection. */
inline constexpr OrderSlot invalid_order_slot =
    (std::numeric_limits<OrderSlot>::max)();

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
  OrderSlot max_orders{};
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

/** @brief Immutable snapshot returned to a matcher after selection. */
struct BestOrderView {
  OrderId id{};
  OwnerId owner_id{};
  PriceTick price{};
  Quantity remaining{};
};

/** @brief First-version change request: reduce remaining quantity only. */
struct OrderChange {
  Quantity new_remaining{};
};

/** @brief Number of bytes/pages touched by a warm-up pass. */
struct WarmUpTouchStats {
  std::size_t bytes{};
  std::size_t pages{};
};

/** @brief Explicit status for put(); capacity failures are system errors. */
enum class PutStatus : std::uint8_t {
  Ok,
  DuplicateOrderId,
  PoolExhausted,
  IndexFull,
  PriceOutOfRange,
  InvalidQuantity
};

/** @brief Explicit status for cancel(). */
enum class CancelStatus : std::uint8_t {
  Ok,
  NotFound
};

/** @brief Explicit status for change(). */
enum class ChangeStatus : std::uint8_t {
  Ok,
  NotFound,
  InvalidQuantity
};

/** @brief Small result wrapper for put(). */
struct PutResult {
  PutStatus status{PutStatus::Ok};

  [[nodiscard]] bool ok() const noexcept {
    return status == PutStatus::Ok;
  }
};

/** @brief Small result wrapper for cancel(). */
struct CancelResult {
  CancelStatus status{CancelStatus::Ok};

  [[nodiscard]] bool ok() const noexcept {
    return status == CancelStatus::Ok;
  }
};

/** @brief Small result wrapper for change(). */
struct ChangeResult {
  ChangeStatus status{ChangeStatus::Ok};

  [[nodiscard]] bool ok() const noexcept {
    return status == ChangeStatus::Ok;
  }
};

} // namespace fexma::order_book
