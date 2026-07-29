#include <fexma/order_book/order_book.hpp>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <random>
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
        (resting_side == Side::Ask
             ? order.price < best->price
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
  std::sort(expected.begin(), expected.end(), [side](const auto& lhs,
                                                     const auto& rhs) {
    if (lhs.price != rhs.price) {
      return side == Side::Ask ? lhs.price < rhs.price : lhs.price > rhs.price;
    }
    return lhs.sequence < rhs.sequence;
  });

  std::vector<BestOrderView> actual;
  book.for_each_order_for_test(side, [&actual](BestOrderView view) noexcept {
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
  if (!book.validate_invariants()) {
    return false;
  }
  if (!compare_side(book, model, Side::Bid) ||
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

} // namespace

int main() {
  std::mt19937 rng(0x0B00C5U);
  OrderBook book({64, 191, 128});
  book.warm_up();
  std::vector<ModelOrder> model;
  std::uint64_t sequence = 0;
  OrderId next_id = 1;

  for (int step = 0; step < 5000; ++step) {
    const int op = static_cast<int>(rng() % 4);
    if ((op == 0 && model.size() < 128) || model.empty()) {
      const Side side = (rng() & 1U) == 0 ? Side::Bid : Side::Ask;
      const PriceTick price = 64 + static_cast<PriceTick>(rng() % 128);
      const Quantity quantity = 1 + static_cast<Quantity>(rng() % 100);
      const OrderId id = next_id++;
      const PutResult result = book.put({id, id + 1000, side, price, quantity});
      if (!result.ok()) {
        return 1;
      }
      model.push_back({id, id + 1000, side, price, quantity, sequence++});
    } else if (op == 1) {
      const std::size_t index = rng() % model.size();
      const OrderId id = model[index].id;
      if (!book.cancel(id).ok()) {
        return 2;
      }
      model.erase(model.begin() + static_cast<std::ptrdiff_t>(index));
    } else if (op == 2) {
      const std::size_t index = rng() % model.size();
      ModelOrder& order = model[index];
      const Quantity new_remaining =
          order.remaining == 1 ? 0 : static_cast<Quantity>(rng() % order.remaining);
      if (!book.change(order.id, {new_remaining}).ok()) {
        return 3;
      }
      if (new_remaining == 0) {
        model.erase(model.begin() + static_cast<std::ptrdiff_t>(index));
      } else {
        order.remaining = new_remaining;
      }
    } else {
      const Side incoming = (rng() & 1U) == 0 ? Side::Bid : Side::Ask;
      const auto expected = model_best(model, incoming);
      const auto actual = book.select_best_opposite(incoming);
      if (actual.has_value() != expected.has_value()) {
        return 4;
      }
      if (actual) {
        if (actual->id != expected->id || actual->price != expected->price ||
            actual->remaining != expected->remaining) {
          return 5;
        }
        const Quantity decrement =
            1 + static_cast<Quantity>(rng() % actual->remaining);
        book.decrement_selected(decrement);
        auto it = std::find_if(model.begin(), model.end(), [actual](const auto& order) {
          return order.id == actual->id;
        });
        if (it == model.end()) {
          return 6;
        }
        it->remaining -= decrement;
        if (it->remaining == 0) {
          model.erase(it);
        }
      }
    }

    if (!compare(book, model)) {
      return 7;
    }
  }

  return 0;
}
