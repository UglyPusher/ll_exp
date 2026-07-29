/**
 * @file test_randomized.cpp
 * @brief Randomized reference-model test for OrderBook structural operations.
 */
#include <fexma/order_book/order_book.hpp>

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

namespace {

struct ModelOrder {
  OrderId id{};
  OwnerId owner_id{};
  Side side{};
  PriceTick price{};
  Quantity remaining{};
  std::uint64_t sequence{};
};

std::optional<ModelOrder> model_best(const std::vector<ModelOrder>& orders,
                                     Side incoming_side) {
  const Side resting_side = incoming_side == Side::Bid ? Side::Ask : Side::Bid;
  std::optional<ModelOrder> best;
  for (const auto& order : orders) {
    if (order.side != resting_side) {
      continue;
    }
    if (!best ||
        (resting_side == Side::Ask ? order.price < best->price
                                   : order.price > best->price) ||
        (order.price == best->price && order.sequence < best->sequence)) {
      best = order;
    }
  }
  return best;
}

bool compare_side(OrderBook& book, const std::vector<ModelOrder>& model,
                  Side side) {
  std::vector<ModelOrder> expected;
  for (const auto& order : model) {
    if (order.side == side) {
      expected.push_back(order);
    }
  }
  std::sort(expected.begin(), expected.end(),
            [side](const auto& lhs, const auto& rhs) {
              if (lhs.price != rhs.price) {
                return side == Side::Ask ? lhs.price < rhs.price
                                         : lhs.price > rhs.price;
              }
              return lhs.sequence < rhs.sequence;
            });

  std::vector<BestOrderView> actual;
  book.for_each_order_for_test(side, [&actual](BestOrderView view) {
    actual.push_back(view);
    return true;
  });

  if (actual.size() != expected.size()) {
    return false;
  }
  for (std::size_t i = 0; i < actual.size(); ++i) {
    if (actual[i].id != expected[i].id ||
        actual[i].owner_id != expected[i].owner_id ||
        actual[i].price != expected[i].price ||
        actual[i].remaining != expected[i].remaining) {
      return false;
    }
  }
  return true;
}

bool compare(OrderBook& book, const std::vector<ModelOrder>& model) {
  if (!book.validate_invariants() || !compare_side(book, model, Side::Bid) ||
      !compare_side(book, model, Side::Ask)) {
    return false;
  }

  const auto expected_bid = model_best(model, Side::Ask);
  const auto expected_ask = model_best(model, Side::Bid);
  if (book.best_bid().has_value() != expected_bid.has_value() ||
      book.best_ask().has_value() != expected_ask.has_value()) {
    return false;
  }
  if (expected_bid && *book.best_bid() != expected_bid->price) {
    return false;
  }
  if (expected_ask && *book.best_ask() != expected_ask->price) {
    return false;
  }
  return true;
}

void record(std::deque<std::string>& history, std::string event) {
  history.push_back(std::move(event));
  if (history.size() > 32) {
    history.pop_front();
  }
}

int fail(std::uint32_t seed, int step, int code,
         const std::deque<std::string>& history,
         const std::vector<ModelOrder>& model) {
  std::cerr << "seed=" << seed << " step=" << step << " code=" << code
            << " active=" << model.size() << '\n';
  for (const auto& event : history) {
    std::cerr << event << '\n';
  }
  return code;
}

} // namespace

int main() {
  const std::uint32_t seeds[] = {0x0B00C5U, 0x12345678U, 0xC0FFEEU,
                                 0xABCDEF01U};

  for (std::uint32_t seed : seeds) {
    std::mt19937 rng(seed);
    OrderBook book({64, 255, 160});
    book.warm_up();
    std::vector<ModelOrder> model;
    std::vector<OrderId> retired_ids;
    std::deque<std::string> history;
    std::uint64_t sequence = 0;
    OrderId next_id = 1;

    for (int step = 0; step < 30000; ++step) {
      const int op = static_cast<int>(rng() % 100);
      if ((op < 35 && model.size() < 150) || model.empty()) {
        const Side side = (rng() & 1U) == 0 ? Side::Bid : Side::Ask;
        const PriceTick boundary_prices[] = {64,  65,  127, 128,
                                             129, 191, 192, 255};
        const PriceTick price =
            (rng() % 4) == 0
                ? boundary_prices[rng() % std::size(boundary_prices)]
                : static_cast<PriceTick>(64 + (rng() % 192));
        const Quantity quantity = 1 + static_cast<Quantity>(rng() % 100);
        OrderId id = next_id++;
        if (!retired_ids.empty() && (rng() % 10) == 0) {
          id = retired_ids.back();
          retired_ids.pop_back();
        }
        const PutResult result = book.put({id, id + 1000, side, price, quantity});
        record(history, "put id=" + std::to_string(id));
        if (!result.ok()) {
          return fail(seed, step, 1, history, model);
        }
        model.push_back({id, id + 1000, side, price, quantity, sequence++});
      } else if (op < 65) {
        const std::size_t index = rng() % model.size();
        const OrderId id = model[index].id;
        record(history, "cancel id=" + std::to_string(id));
        if (!book.cancel(id).ok()) {
          return fail(seed, step, 2, history, model);
        }
        retired_ids.push_back(id);
        model.erase(model.begin() + static_cast<std::ptrdiff_t>(index));
      } else if (op < 90) {
        const std::size_t index = rng() % model.size();
        ModelOrder& order = model[index];
        const Quantity new_remaining =
            order.remaining == 1
                ? 0
                : static_cast<Quantity>(rng() % (order.remaining + 1));
        record(history, "change id=" + std::to_string(order.id) +
                            " qty=" + std::to_string(new_remaining));
        const ChangeResult result = book.change(order.id, {new_remaining});
        if (new_remaining >= order.remaining) {
          if (result.status != ChangeStatus::InvalidQuantity) {
            return fail(seed, step, 3, history, model);
          }
        } else {
          if (!result.ok()) {
            return fail(seed, step, 4, history, model);
          }
          if (new_remaining == 0) {
            retired_ids.push_back(order.id);
            model.erase(model.begin() + static_cast<std::ptrdiff_t>(index));
          } else {
            order.remaining = new_remaining;
          }
        }
      } else {
        const Side incoming = (rng() & 1U) == 0 ? Side::Bid : Side::Ask;
        const auto expected = model_best(model, incoming);
        const auto actual = book.select_best_opposite(incoming);
        record(history, "select incoming=" +
                            std::to_string(static_cast<int>(incoming)));
        if (actual.has_value() != expected.has_value()) {
          return fail(seed, step, 5, history, model);
        }
        if (actual) {
          if (actual->id != expected->id || actual->price != expected->price ||
              actual->remaining != expected->remaining) {
            return fail(seed, step, 6, history, model);
          }
          if ((rng() % 3) == 0) {
            const OrderId other = model[rng() % model.size()].id;
            if (other != actual->id) {
              (void)book.cancel(other);
              auto it = std::find_if(model.begin(), model.end(),
                                     [other](const auto& order) {
                                       return order.id == other;
                                     });
              if (it != model.end()) {
                retired_ids.push_back(it->id);
                model.erase(it);
              }
              record(history, "cancel-other-after-select");
            }
          } else {
            const Quantity decrement =
                1 + static_cast<Quantity>(rng() % actual->remaining);
            book.decrement_selected(decrement);
            auto it = std::find_if(model.begin(), model.end(),
                                   [actual](const auto& order) {
                                     return order.id == actual->id;
                                   });
            if (it == model.end()) {
              return fail(seed, step, 7, history, model);
            }
            it->remaining -= decrement;
            if (it->remaining == 0) {
              retired_ids.push_back(it->id);
              model.erase(it);
            }
            record(history, "decrement-selected");
          }
        }
      }

      if (!compare(book, model)) {
        return fail(seed, step, 8, history, model);
      }
    }
  }

  return 0;
}
