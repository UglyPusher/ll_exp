/**
 * @file basic_usage.cpp
 * @brief Minimal use of the public OrderBook API.
 */
#include <fexma/order_book/order_book.hpp>

#include <iostream>

using namespace fexma::order_book;

int main() {
  OrderBook book({100, 200, 8});

  const InsertResult bid_inserted = book.insert({1, 101, Side::Bid, 150, 10});
  const InsertResult ask_inserted = book.insert({2, 202, Side::Ask, 155, 12});
  if (!bid_inserted.ok() || !ask_inserted.ok()) {
    std::cerr << "insert failed\n";
    return 1;
  }

  const auto best_bid = book.best(Side::Bid);
  const auto best_ask = book.best(Side::Ask);
  if (!best_bid || best_bid->id != 1 || !best_ask || best_ask->id != 2) {
    std::cerr << "unexpected best order\n";
    return 2;
  }

  const SetRemainingResult changed = book.set_remaining(1, 7);
  if (!changed.ok() || changed.previous_remaining != 10) {
    std::cerr << "set_remaining failed\n";
    return 3;
  }

  const EraseResult erased = book.erase(2);
  if (!erased.ok() || erased.removed.id != 2) {
    std::cerr << "erase failed\n";
    return 4;
  }

  if (!book.validate_invariants()) {
    std::cerr << "invariant validation failed\n";
    return 5;
  }

  std::cout << "best_bid_id=" << book.best(Side::Bid)->id
            << " remaining=" << book.best(Side::Bid)->remaining
            << " ask_empty=" << !book.best(Side::Ask).has_value() << '\n';
  return 0;
}
