/**
 * @file test_order_book.cpp
 * @brief Black-box OrderBook API tests for the target five-method contract.
 */
#include <fexma/order_book/order_book.hpp>

#include <cstddef>
#include <cstdlib>
#include <new>
#include <optional>

#ifdef _MSC_VER
#include <malloc.h>
#endif

using namespace fexma::order_book;

namespace {

inline std::uint64_t g_allocations = 0;
inline bool g_count_allocations = false;

[[nodiscard]] bool same_order(const OrderView& lhs,
                              const OrderView& rhs) noexcept {
  return lhs.id == rhs.id && lhs.owner_id == rhs.owner_id &&
         lhs.side == rhs.side && lhs.price == rhs.price &&
         lhs.remaining == rhs.remaining;
}

[[nodiscard]] bool same_optional_order(
    const std::optional<OrderView>& lhs,
    const std::optional<OrderView>& rhs) noexcept {
  if (lhs.has_value() != rhs.has_value()) {
    return false;
  }
  return !lhs || same_order(*lhs, *rhs);
}

[[nodiscard]] bool unchanged_after_failed_insert(
    OrderBook& book, const RestingOrderData& order,
    InsertStatus expected_status) {
  const auto best_bid_before = book.best(Side::Bid);
  const auto best_ask_before = book.best(Side::Ask);

  const InsertResult result = book.insert(order);

  return result.status == expected_status &&
         same_optional_order(book.best(Side::Bid), best_bid_before) &&
         same_optional_order(book.best(Side::Ask), best_ask_before) &&
         book.validate_invariants();
}

[[nodiscard]] bool unchanged_after_failed_set_remaining(
    OrderBook& book, OrderId id, Quantity new_remaining,
    SetRemainingStatus expected_status) {
  const auto best_bid_before = book.best(Side::Bid);
  const auto best_ask_before = book.best(Side::Ask);

  const SetRemainingResult result = book.set_remaining(id, new_remaining);

  return result.status == expected_status &&
         same_optional_order(book.best(Side::Bid), best_bid_before) &&
         same_optional_order(book.best(Side::Ask), best_ask_before) &&
         book.validate_invariants();
}

[[nodiscard]] bool unchanged_after_failed_erase(
    OrderBook& book, OrderId id, EraseStatus expected_status) {
  const auto best_bid_before = book.best(Side::Bid);
  const auto best_ask_before = book.best(Side::Ask);

  const EraseResult result = book.erase(id);

  return result.status == expected_status &&
         same_optional_order(book.best(Side::Bid), best_bid_before) &&
         same_optional_order(book.best(Side::Ask), best_ask_before) &&
         book.validate_invariants();
}

void reset_allocation_counter() noexcept {
  g_allocations = 0;
  g_count_allocations = true;
}

std::uint64_t stop_allocation_counter() noexcept {
  g_count_allocations = false;
  return g_allocations;
}

void* counted_alloc(std::size_t size, std::size_t alignment = 0) {
  if (g_count_allocations) {
    ++g_allocations;
  }
  if (alignment == 0) {
    if (void* ptr = std::malloc(size)) {
      return ptr;
    }
  } else {
    const std::size_t rounded = ((size + alignment - 1) / alignment) * alignment;
#ifdef _MSC_VER
    if (void* ptr = _aligned_malloc(rounded, alignment)) {
      return ptr;
    }
#else
    if (void* ptr = std::aligned_alloc(alignment, rounded)) {
      return ptr;
    }
#endif
  }
  throw std::bad_alloc();
}

[[nodiscard]] bool construction_contract() {
  OrderBook empty_capacity({10, 20, 0});
  if (empty_capacity.best(Side::Bid) || empty_capacity.best(Side::Ask) ||
      !empty_capacity.validate_invariants()) {
    return false;
  }
  if (empty_capacity.insert({1, 1, Side::Bid, 10, 1}).status !=
      InsertStatus::CapacityExhausted) {
    return false;
  }

  OrderBook single_capacity({10, 10, 1});
  return single_capacity.insert({1, 1, Side::Ask, 10, 1}).ok() &&
         single_capacity.insert({2, 2, Side::Ask, 10, 1}).status ==
             InsertStatus::CapacityExhausted &&
         single_capacity.validate_invariants();
}

[[nodiscard]] bool insert_and_best_contract() {
  OrderBook book({64, 192, 8});
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
  if (!best_bid || !same_order(*best_bid, {4, 14, Side::Bid, 191, 40}) ||
      !best_ask || !same_order(*best_ask, {8, 18, Side::Ask, 64, 80})) {
    return false;
  }

  return unchanged_after_failed_insert(book, {1, 99, Side::Ask, 150, 1},
                                       InsertStatus::DuplicateOrderId) &&
         unchanged_after_failed_insert(book, {9, 19, Side::Bid, 63, 1},
                                       InsertStatus::PriceOutOfRange) &&
         unchanged_after_failed_insert(book, {9, 19, Side::Ask, 193, 1},
                                       InsertStatus::PriceOutOfRange) &&
         unchanged_after_failed_insert(book, {9, 19, Side::Bid, 100, 0},
                                       InsertStatus::InvalidQuantity);
}

[[nodiscard]] bool set_remaining_contract() {
  OrderBook book({10, 20, 4});
  if (!book.insert({1, 11, Side::Ask, 12, 10}).ok() ||
      !book.insert({2, 12, Side::Ask, 12, 20}).ok()) {
    return false;
  }

  const SetRemainingResult lower = book.set_remaining(1, 5);
  if (!lower.ok() || lower.previous_remaining != 10 ||
      book.best(Side::Ask)->id != 1 ||
      book.best(Side::Ask)->remaining != 5) {
    return false;
  }

  const SetRemainingResult higher = book.set_remaining(1, 30);
  if (!higher.ok() || higher.previous_remaining != 5 ||
      book.best(Side::Ask)->id != 1 ||
      book.best(Side::Ask)->remaining != 30) {
    return false;
  }

  const SetRemainingResult same = book.set_remaining(1, 30);
  return same.ok() && same.previous_remaining == 30 &&
         unchanged_after_failed_set_remaining(
             book, 999, 1, SetRemainingStatus::NotFound) &&
         unchanged_after_failed_set_remaining(
             book, 1, 0, SetRemainingStatus::InvalidQuantity);
}

[[nodiscard]] bool erase_contract() {
  OrderBook book({10, 20, 5});
  if (!book.insert({1, 11, Side::Bid, 15, 10}).ok() ||
      !book.insert({2, 12, Side::Bid, 15, 20}).ok() ||
      !book.insert({3, 13, Side::Bid, 15, 30}).ok() ||
      !book.insert({4, 14, Side::Bid, 10, 40}).ok()) {
    return false;
  }

  const EraseResult middle = book.erase(2);
  if (!middle.ok() ||
      !same_order(middle.removed, {2, 12, Side::Bid, 15, 20}) ||
      book.best(Side::Bid)->id != 1) {
    return false;
  }

  const EraseResult head = book.erase(1);
  if (!head.ok() || book.best(Side::Bid)->id != 3) {
    return false;
  }

  const EraseResult tail = book.erase(3);
  if (!tail.ok() || book.best(Side::Bid)->id != 4) {
    return false;
  }

  if (!unchanged_after_failed_erase(book, 999, EraseStatus::NotFound) ||
      !unchanged_after_failed_erase(book, 3, EraseStatus::NotFound) ||
      !book.erase(4).ok() || book.best(Side::Bid)) {
    return false;
  }

  return book.insert({1, 11, Side::Ask, 20, 10}).ok() &&
         book.validate_invariants();
}

[[nodiscard]] bool runtime_operations_do_not_allocate() {
  OrderBook book({1, 128, 16});
  (void)book.insert({1, 1, Side::Ask, 10, 10});
  (void)book.insert({2, 1, Side::Bid, 9, 10});

  reset_allocation_counter();
  (void)book.insert({3, 1, Side::Ask, 10, 10});
  if (stop_allocation_counter() != 0) {
    return false;
  }

  reset_allocation_counter();
  const auto best_ask = book.best(Side::Ask);
  if (stop_allocation_counter() != 0 || !best_ask) {
    return false;
  }

  reset_allocation_counter();
  (void)book.set_remaining(best_ask->id, 5);
  if (stop_allocation_counter() != 0) {
    return false;
  }

  reset_allocation_counter();
  (void)book.erase(2);
  return stop_allocation_counter() == 0 && book.validate_invariants();
}

} // namespace

void* operator new(std::size_t size) {
  return counted_alloc(size);
}

void* operator new[](std::size_t size) {
  return counted_alloc(size);
}

void* operator new(std::size_t size, std::align_val_t alignment) {
  return counted_alloc(size, static_cast<std::size_t>(alignment));
}

void* operator new[](std::size_t size, std::align_val_t alignment) {
  return counted_alloc(size, static_cast<std::size_t>(alignment));
}

void operator delete(void* ptr) noexcept {
  std::free(ptr);
}

void operator delete[](void* ptr) noexcept {
  std::free(ptr);
}

void operator delete(void* ptr, std::size_t) noexcept {
  std::free(ptr);
}

void operator delete[](void* ptr, std::size_t) noexcept {
  std::free(ptr);
}

void operator delete(void* ptr, std::align_val_t) noexcept {
#ifdef _MSC_VER
  _aligned_free(ptr);
#else
  std::free(ptr);
#endif
}

void operator delete[](void* ptr, std::align_val_t) noexcept {
#ifdef _MSC_VER
  _aligned_free(ptr);
#else
  std::free(ptr);
#endif
}

void operator delete(void* ptr, std::size_t, std::align_val_t) noexcept {
#ifdef _MSC_VER
  _aligned_free(ptr);
#else
  std::free(ptr);
#endif
}

void operator delete[](void* ptr, std::size_t, std::align_val_t) noexcept {
#ifdef _MSC_VER
  _aligned_free(ptr);
#else
  std::free(ptr);
#endif
}

int main() {
  if (!construction_contract()) {
    return 1;
  }
  if (!insert_and_best_contract()) {
    return 2;
  }
  if (!set_remaining_contract()) {
    return 3;
  }
  if (!erase_contract()) {
    return 4;
  }
  if (!runtime_operations_do_not_allocate()) {
    return 5;
  }
  return 0;
}
