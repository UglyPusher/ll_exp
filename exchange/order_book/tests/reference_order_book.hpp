/**
 * @file reference_order_book.hpp
 * @brief Test-only reference model for the target OrderBook contract.
 */
#pragma once

#include <fexma/order_book/types.hpp>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <iterator>
#include <map>
#include <optional>
#include <unordered_map>
#include <unordered_set>

namespace fexma::order_book::test_support {

struct ReferenceOrderView {
  OrderId id{};
  OwnerId owner_id{};
  Side side{};
  PriceTick price{};
  Quantity remaining{};
};

enum class ReferenceInsertStatus : std::uint8_t {
  Ok,
  DuplicateOrderId,
  CapacityExhausted,
  PriceOutOfRange,
  InvalidQuantity
};

struct ReferenceInsertResult {
  ReferenceInsertStatus status{ReferenceInsertStatus::Ok};

  [[nodiscard]] bool ok() const noexcept {
    return status == ReferenceInsertStatus::Ok;
  }
};

enum class ReferenceSetRemainingStatus : std::uint8_t {
  Ok,
  NotFound,
  InvalidQuantity
};

struct ReferenceSetRemainingResult {
  ReferenceSetRemainingStatus status{ReferenceSetRemainingStatus::Ok};
  Quantity previous_remaining{};

  [[nodiscard]] bool ok() const noexcept {
    return status == ReferenceSetRemainingStatus::Ok;
  }
};

enum class ReferenceEraseStatus : std::uint8_t {
  Ok,
  NotFound
};

struct ReferenceEraseResult {
  ReferenceEraseStatus status{ReferenceEraseStatus::Ok};
  ReferenceOrderView removed{};

  [[nodiscard]] bool ok() const noexcept {
    return status == ReferenceEraseStatus::Ok;
  }
};

/**
 * @brief Slow, obvious oracle for the intended five-method OrderBook API.
 *
 * This model intentionally uses standard containers and simple walks. It is not
 * a production implementation candidate; it exists to make contract tests and
 * later differential tests easy to reason about.
 */
class ReferenceOrderBook {
public:
  explicit ReferenceOrderBook(OrderBookConfig config) : config_(config) {}

  [[nodiscard]] ReferenceInsertResult
  insert(const RestingOrderData& order) {
    if (order.quantity == 0) {
      return {ReferenceInsertStatus::InvalidQuantity};
    }
    if (!price_in_range(order.price)) {
      return {ReferenceInsertStatus::PriceOutOfRange};
    }
    if (orders_by_id_.contains(order.id)) {
      return {ReferenceInsertStatus::DuplicateOrderId};
    }
    if (orders_by_id_.size() >= config_.max_orders) {
      return {ReferenceInsertStatus::CapacityExhausted};
    }

    orders_by_id_.emplace(order.id,
                          StoredOrder{order.id, order.owner_id, order.side,
                                      order.price, order.quantity});
    price_levels(order.side)[order.price].push_back(order.id);
    return {ReferenceInsertStatus::Ok};
  }

  [[nodiscard]] std::optional<ReferenceOrderView> best(Side side) const {
    const auto& levels = price_levels(side);
    if (levels.empty()) {
      return std::nullopt;
    }

    const auto level =
        side == Side::Bid ? std::prev(levels.end()) : levels.begin();
    if (level->second.empty()) {
      return std::nullopt;
    }
    return view_for(level->second.front());
  }

  [[nodiscard]] ReferenceSetRemainingResult
  set_remaining(OrderId id, Quantity new_remaining) {
    if (new_remaining == 0) {
      return {ReferenceSetRemainingStatus::InvalidQuantity, {}};
    }

    auto order = orders_by_id_.find(id);
    if (order == orders_by_id_.end()) {
      return {ReferenceSetRemainingStatus::NotFound, {}};
    }

    const Quantity previous_remaining = order->second.remaining;
    order->second.remaining = new_remaining;
    return {ReferenceSetRemainingStatus::Ok, previous_remaining};
  }

  [[nodiscard]] ReferenceEraseResult erase(OrderId id) {
    auto order = orders_by_id_.find(id);
    if (order == orders_by_id_.end()) {
      return {ReferenceEraseStatus::NotFound, {}};
    }

    const ReferenceOrderView removed = view_from(order->second);
    auto& levels = price_levels(order->second.side);
    auto level = levels.find(order->second.price);
    if (level == levels.end()) {
      return {ReferenceEraseStatus::NotFound, {}};
    }

    auto& order_ids_at_price = level->second;
    for (auto current = order_ids_at_price.begin();
         current != order_ids_at_price.end(); ++current) {
      if (*current == id) {
        order_ids_at_price.erase(current);
        break;
      }
    }
    if (order_ids_at_price.empty()) {
      levels.erase(level);
    }
    orders_by_id_.erase(order);
    return {ReferenceEraseStatus::Ok, removed};
  }

  [[nodiscard]] bool validate_invariants() const {
    if (config_.max_price_tick < config_.min_price_tick) {
      return orders_by_id_.empty() && bid_levels_.empty() &&
             ask_levels_.empty();
    }
    if (orders_by_id_.size() > config_.max_orders) {
      return false;
    }

    std::unordered_set<OrderId> ids_seen_in_levels;
    ids_seen_in_levels.reserve(orders_by_id_.size());
    if (!validate_side(Side::Bid, bid_levels_, ids_seen_in_levels) ||
        !validate_side(Side::Ask, ask_levels_, ids_seen_in_levels)) {
      return false;
    }

    if (ids_seen_in_levels.size() != orders_by_id_.size()) {
      return false;
    }
    for (const auto& [id, order] : orders_by_id_) {
      if (order.id != id || order.remaining == 0 ||
          !price_in_range(order.price) || !ids_seen_in_levels.contains(id)) {
        return false;
      }
    }
    return true;
  }

  [[nodiscard]] std::size_t active_order_count() const noexcept {
    return orders_by_id_.size();
  }

private:
  struct StoredOrder {
    OrderId id{};
    OwnerId owner_id{};
    Side side{};
    PriceTick price{};
    Quantity remaining{};
  };

  using PriceLevels = std::map<PriceTick, std::deque<OrderId>>;

  [[nodiscard]] bool price_in_range(PriceTick price) const noexcept {
    return price >= config_.min_price_tick && price <= config_.max_price_tick;
  }

  [[nodiscard]] PriceLevels& price_levels(Side side) noexcept {
    return side == Side::Bid ? bid_levels_ : ask_levels_;
  }

  [[nodiscard]] const PriceLevels& price_levels(Side side) const noexcept {
    return side == Side::Bid ? bid_levels_ : ask_levels_;
  }

  [[nodiscard]] ReferenceOrderView view_for(OrderId id) const {
    return view_from(orders_by_id_.at(id));
  }

  [[nodiscard]] static ReferenceOrderView view_from(const StoredOrder& order) {
    return {order.id, order.owner_id, order.side, order.price,
            order.remaining};
  }

  [[nodiscard]] bool validate_side(
      Side side, const PriceLevels& levels,
      std::unordered_set<OrderId>& ids_seen_in_levels) const {
    for (const auto& [price, order_ids] : levels) {
      if (!price_in_range(price) || order_ids.empty()) {
        return false;
      }
      for (OrderId id : order_ids) {
        const auto order = orders_by_id_.find(id);
        if (order == orders_by_id_.end() || order->second.side != side ||
            order->second.price != price || order->second.remaining == 0 ||
            !ids_seen_in_levels.insert(id).second) {
          return false;
        }
      }
    }
    return true;
  }

  OrderBookConfig config_{};
  std::unordered_map<OrderId, StoredOrder> orders_by_id_;
  PriceLevels bid_levels_;
  PriceLevels ask_levels_;
};

} // namespace fexma::order_book::test_support
