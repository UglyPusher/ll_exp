/**
 * @file test_randomized.cpp
 * @brief Randomized differential test for OrderBook structural operations.
 */
#include <fexma/order_book/order_book.hpp>

#include "reference_order_book.hpp"

#include <algorithm>
#include <cstdint>
#include <deque>
#include <iostream>
#include <iterator>
#include <optional>
#include <random>
#include <string>
#include <vector>

using namespace fexma::order_book;
using namespace fexma::order_book::test_support;

namespace {

[[nodiscard]] bool same_order(const OrderView& lhs,
                              const ReferenceOrderView& rhs) noexcept {
  return lhs.id == rhs.id && lhs.owner_id == rhs.owner_id &&
         lhs.side == rhs.side && lhs.price == rhs.price &&
         lhs.remaining == rhs.remaining;
}

[[nodiscard]] bool same_best(const std::optional<OrderView>& actual,
                             const std::optional<ReferenceOrderView>& expected) {
  if (actual.has_value() != expected.has_value()) {
    return false;
  }
  return !actual || same_order(*actual, *expected);
}

[[nodiscard]] bool same_best_prices(OrderBook& book,
                                    const ReferenceOrderBook& reference) {
  return same_best(book.best(Side::Bid), reference.best(Side::Bid)) &&
         same_best(book.best(Side::Ask), reference.best(Side::Ask));
}

[[nodiscard]] bool same_insert_status(InsertStatus actual,
                                      ReferenceInsertStatus expected) noexcept {
  switch (expected) {
  case ReferenceInsertStatus::Ok:
    return actual == InsertStatus::Ok;
  case ReferenceInsertStatus::DuplicateOrderId:
    return actual == InsertStatus::DuplicateOrderId;
  case ReferenceInsertStatus::CapacityExhausted:
    return actual == InsertStatus::CapacityExhausted;
  case ReferenceInsertStatus::PriceOutOfRange:
    return actual == InsertStatus::PriceOutOfRange;
  case ReferenceInsertStatus::InvalidQuantity:
    return actual == InsertStatus::InvalidQuantity;
  }
  return false;
}

[[nodiscard]] bool same_set_remaining_status(
    SetRemainingStatus actual, ReferenceSetRemainingStatus expected) noexcept {
  switch (expected) {
  case ReferenceSetRemainingStatus::Ok:
    return actual == SetRemainingStatus::Ok;
  case ReferenceSetRemainingStatus::NotFound:
    return actual == SetRemainingStatus::NotFound;
  case ReferenceSetRemainingStatus::InvalidQuantity:
    return actual == SetRemainingStatus::InvalidQuantity;
  }
  return false;
}

[[nodiscard]] bool same_erase_status(EraseStatus actual,
                                     ReferenceEraseStatus expected) noexcept {
  switch (expected) {
  case ReferenceEraseStatus::Ok:
    return actual == EraseStatus::Ok;
  case ReferenceEraseStatus::NotFound:
    return actual == EraseStatus::NotFound;
  }
  return false;
}

void record(std::deque<std::string>& history, std::string event) {
  history.push_back(std::move(event));
  if (history.size() > 32) {
    history.pop_front();
  }
}

int fail(std::uint32_t seed, int step, int code,
         const std::deque<std::string>& history) {
  std::cerr << "seed=" << seed << " step=" << step << " code=" << code
            << '\n';
  for (const auto& event : history) {
    std::cerr << event << '\n';
  }
  return code;
}

void remove_active_id(std::vector<OrderId>& active_ids, OrderId id) {
  const auto position = std::find(active_ids.begin(), active_ids.end(), id);
  if (position != active_ids.end()) {
    active_ids.erase(position);
  }
}

} // namespace

int main() {
  const std::uint32_t seeds[] = {0x0B00C5U, 0x12345678U, 0xC0FFEEU,
                                 0xABCDEF01U};

  for (std::uint32_t seed : seeds) {
    std::mt19937 rng(seed);
    constexpr OrderCapacity max_orders = 160;
    OrderBook book({64, 255, max_orders});
    ReferenceOrderBook reference({64, 255, max_orders});
    std::vector<OrderId> active_ids;
    std::vector<OrderId> retired_ids;
    std::deque<std::string> history;
    OrderId next_id = 1;

    for (int step = 0; step < 30000; ++step) {
      const int op = static_cast<int>(rng() % 100);
      if (op < 40 || active_ids.empty()) {
        const Side side = (rng() & 1U) == 0 ? Side::Bid : Side::Ask;
        const PriceTick boundary_prices[] = {63, 64,  65,  127, 128,
                                             129, 191, 192, 255, 256};
        const PriceTick price =
            (rng() % 4) == 0
                ? boundary_prices[rng() % std::size(boundary_prices)]
                : static_cast<PriceTick>(64 + (rng() % 192));
        const Quantity quantity =
            (rng() % 20) == 0 ? 0 : 1 + static_cast<Quantity>(rng() % 100);
        OrderId id = next_id++;
        if (!active_ids.empty() && (rng() % 8) == 0) {
          id = active_ids[rng() % active_ids.size()];
        } else if (!retired_ids.empty() && (rng() % 10) == 0) {
          id = retired_ids.back();
          retired_ids.pop_back();
        }

        const RestingOrderData order{id, id + 1000, side, price, quantity};
        const InsertResult actual = book.insert(order);
        const ReferenceInsertResult expected = reference.insert(order);
        record(history, "insert id=" + std::to_string(id));
        if (!same_insert_status(actual.status, expected.status)) {
          return fail(seed, step, 1, history);
        }
        if (actual.ok()) {
          active_ids.push_back(id);
        }
      } else if (op < 65) {
        const bool use_missing = (rng() % 5) == 0;
        const OrderId id =
            use_missing ? next_id + 100000
                        : active_ids[rng() % active_ids.size()];
        const Quantity new_remaining =
            (rng() % 12) == 0 ? 0 : 1 + static_cast<Quantity>(rng() % 200);

        const SetRemainingResult actual =
            book.set_remaining(id, new_remaining);
        const ReferenceSetRemainingResult expected =
            reference.set_remaining(id, new_remaining);
        record(history, "set_remaining id=" + std::to_string(id) +
                            " qty=" + std::to_string(new_remaining));
        if (!same_set_remaining_status(actual.status, expected.status) ||
            actual.previous_remaining != expected.previous_remaining) {
          return fail(seed, step, 2, history);
        }
      } else if (op < 90) {
        const bool use_missing = (rng() % 5) == 0;
        const OrderId id =
            use_missing ? next_id + 200000
                        : active_ids[rng() % active_ids.size()];

        const EraseResult actual = book.erase(id);
        const ReferenceEraseResult expected = reference.erase(id);
        record(history, "erase id=" + std::to_string(id));
        if (!same_erase_status(actual.status, expected.status)) {
          return fail(seed, step, 3, history);
        }
        if (actual.ok()) {
          if (!same_order(actual.removed, expected.removed)) {
            return fail(seed, step, 4, history);
          }
          retired_ids.push_back(id);
          remove_active_id(active_ids, id);
        }
      } else {
        const Side side = (rng() & 1U) == 0 ? Side::Bid : Side::Ask;
        record(history, "best side=" + std::to_string(static_cast<int>(side)));
        if (!same_best(book.best(side), reference.best(side))) {
          return fail(seed, step, 5, history);
        }
      }

      if (!book.validate_invariants() || !reference.validate_invariants() ||
          !same_best_prices(book, reference)) {
        return fail(seed, step, 6, history);
      }
    }
  }

  return 0;
}
