/**
 * @file test_reference_order_book.cpp
 * @brief Contract tests for the test-only ReferenceOrderBook oracle.
 */
#include "reference_order_book.hpp"

#include <optional>

using namespace fexma::order_book;
using namespace fexma::order_book::test_support;

namespace {

[[nodiscard]] bool same_order(const ReferenceOrderView& lhs,
                              const ReferenceOrderView& rhs) noexcept {
  return lhs.id == rhs.id && lhs.owner_id == rhs.owner_id &&
         lhs.side == rhs.side && lhs.price == rhs.price &&
         lhs.remaining == rhs.remaining;
}

[[nodiscard]] bool unchanged_after_failed_insert(
    ReferenceOrderBook& book, const RestingOrderData& order,
    ReferenceInsertStatus expected_status) {
  const auto best_bid_before = book.best(Side::Bid);
  const auto best_ask_before = book.best(Side::Ask);
  const std::size_t count_before = book.active_order_count();

  const ReferenceInsertResult result = book.insert(order);

  return result.status == expected_status &&
         book.best(Side::Bid).has_value() == best_bid_before.has_value() &&
         book.best(Side::Ask).has_value() == best_ask_before.has_value() &&
         (!best_bid_before ||
          same_order(*book.best(Side::Bid), *best_bid_before)) &&
         (!best_ask_before ||
          same_order(*book.best(Side::Ask), *best_ask_before)) &&
         book.active_order_count() == count_before &&
         book.validate_invariants();
}

[[nodiscard]] bool construction_contract() {
  ReferenceOrderBook empty_capacity({10, 20, 0});
  if (empty_capacity.best(Side::Bid) || empty_capacity.best(Side::Ask) ||
      !empty_capacity.validate_invariants()) {
    return false;
  }
  if (empty_capacity.insert({1, 1, Side::Bid, 10, 1}).status !=
      ReferenceInsertStatus::CapacityExhausted) {
    return false;
  }

  ReferenceOrderBook invalid_range({20, 10, 1});
  if (invalid_range.insert({1, 1, Side::Bid, 15, 1}).status !=
          ReferenceInsertStatus::PriceOutOfRange ||
      !invalid_range.validate_invariants()) {
    return false;
  }

  ReferenceOrderBook single_capacity({10, 10, 1});
  return single_capacity.insert({1, 1, Side::Ask, 10, 1}).ok() &&
         single_capacity.insert({2, 2, Side::Ask, 10, 1}).status ==
             ReferenceInsertStatus::CapacityExhausted &&
         single_capacity.validate_invariants();
}

[[nodiscard]] bool insert_contract() {
  ReferenceOrderBook book({100, 200, 2});
  if (!book.insert({1, 11, Side::Bid, 100, 10}).ok() ||
      !book.insert({2, 12, Side::Ask, 200, 20}).ok()) {
    return false;
  }

  return unchanged_after_failed_insert(book, {1, 99, Side::Ask, 150, 1},
                                       ReferenceInsertStatus::DuplicateOrderId) &&
         unchanged_after_failed_insert(book, {3, 13, Side::Bid, 99, 1},
                                       ReferenceInsertStatus::PriceOutOfRange) &&
         unchanged_after_failed_insert(book, {3, 13, Side::Ask, 201, 1},
                                       ReferenceInsertStatus::PriceOutOfRange) &&
         unchanged_after_failed_insert(book, {3, 13, Side::Bid, 150, 0},
                                       ReferenceInsertStatus::InvalidQuantity) &&
         unchanged_after_failed_insert(book, {3, 13, Side::Bid, 150, 1},
                                       ReferenceInsertStatus::CapacityExhausted);
}

[[nodiscard]] bool price_and_fifo_priority_contract() {
  ReferenceOrderBook book({64, 192, 8});
  if (!book.insert({1, 11, Side::Bid, 65, 10}).ok() ||
      !book.insert({2, 12, Side::Bid, 128, 20}).ok() ||
      !book.insert({3, 13, Side::Bid, 128, 30}).ok() ||
      !book.insert({4, 14, Side::Bid, 191, 40}).ok() ||
      !book.insert({5, 15, Side::Ask, 191, 50}).ok() ||
      !book.insert({6, 16, Side::Ask, 129, 60}).ok() ||
      !book.insert({7, 17, Side::Ask, 129, 70}).ok() ||
      !book.insert({8, 18, Side::Ask, 64, 80}).ok()) {
    return false;
  }

  const auto best_bid = book.best(Side::Bid);
  const auto best_ask = book.best(Side::Ask);
  if (!best_bid || best_bid->id != 4 || best_bid->price != 191 ||
      !best_ask || best_ask->id != 8 || best_ask->price != 64) {
    return false;
  }

  if (!book.erase(4).ok()) {
    return false;
  }
  const auto next_best_bid = book.best(Side::Bid);
  if (!next_best_bid || next_best_bid->id != 2 ||
      next_best_bid->price != 128) {
    return false;
  }

  if (!book.erase(8).ok()) {
    return false;
  }
  const auto next_best_ask = book.best(Side::Ask);
  return next_best_ask && next_best_ask->id == 6 &&
         next_best_ask->price == 129 && book.validate_invariants();
}

[[nodiscard]] bool set_remaining_contract() {
  ReferenceOrderBook book({10, 20, 4});
  if (!book.insert({1, 11, Side::Ask, 12, 10}).ok() ||
      !book.insert({2, 12, Side::Ask, 12, 20}).ok()) {
    return false;
  }

  const ReferenceSetRemainingResult lower = book.set_remaining(1, 5);
  const auto best_after_lower = book.best(Side::Ask);
  if (!lower.ok() || lower.previous_remaining != 10 || !best_after_lower ||
      best_after_lower->id != 1 || best_after_lower->remaining != 5) {
    return false;
  }

  const ReferenceSetRemainingResult higher = book.set_remaining(1, 30);
  const auto best_after_higher = book.best(Side::Ask);
  if (!higher.ok() || higher.previous_remaining != 5 || !best_after_higher ||
      best_after_higher->id != 1 || best_after_higher->remaining != 30) {
    return false;
  }

  const ReferenceSetRemainingResult same = book.set_remaining(1, 30);
  if (!same.ok() || same.previous_remaining != 30 ||
      book.best(Side::Ask)->id != 1) {
    return false;
  }

  const auto best_before_misses = book.best(Side::Ask);
  return book.set_remaining(999, 1).status ==
             ReferenceSetRemainingStatus::NotFound &&
         book.set_remaining(1, 0).status ==
             ReferenceSetRemainingStatus::InvalidQuantity &&
         best_before_misses && book.best(Side::Ask) &&
         same_order(*best_before_misses, *book.best(Side::Ask)) &&
         book.validate_invariants();
}

[[nodiscard]] bool erase_contract() {
  ReferenceOrderBook book({10, 20, 5});
  if (!book.insert({1, 11, Side::Bid, 15, 10}).ok() ||
      !book.insert({2, 12, Side::Bid, 15, 20}).ok() ||
      !book.insert({3, 13, Side::Bid, 15, 30}).ok() ||
      !book.insert({4, 14, Side::Bid, 10, 40}).ok()) {
    return false;
  }

  const ReferenceEraseResult middle = book.erase(2);
  if (!middle.ok() ||
      !same_order(middle.removed, {2, 12, Side::Bid, 15, 20}) ||
      book.best(Side::Bid)->id != 1) {
    return false;
  }

  const ReferenceEraseResult head = book.erase(1);
  if (!head.ok() || book.best(Side::Bid)->id != 3) {
    return false;
  }

  const ReferenceEraseResult tail = book.erase(3);
  if (!tail.ok() || book.best(Side::Bid)->id != 4) {
    return false;
  }

  if (book.erase(999).status != ReferenceEraseStatus::NotFound ||
      book.erase(3).status != ReferenceEraseStatus::NotFound ||
      !book.erase(4).ok() || book.best(Side::Bid)) {
    return false;
  }

  return book.insert({1, 11, Side::Ask, 20, 10}).ok() &&
         book.active_order_count() == 1 && book.validate_invariants();
}

} // namespace

int main() {
  if (!construction_contract()) {
    return 1;
  }
  if (!insert_contract()) {
    return 2;
  }
  if (!price_and_fifo_priority_contract()) {
    return 3;
  }
  if (!set_remaining_contract()) {
    return 4;
  }
  if (!erase_contract()) {
    return 5;
  }
  return 0;
}
