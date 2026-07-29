#include <fexma/order_book/order_book.hpp>

#include <cstdlib>
#include <new>

using namespace fexma::order_book;

namespace {

inline std::uint64_t g_allocations = 0;
inline bool g_count_allocations = false;

bool expect(bool condition) noexcept {
  return condition;
}

void reset_allocation_counter() noexcept {
  g_allocations = 0;
  g_count_allocations = true;
}

std::uint64_t stop_allocation_counter() noexcept {
  g_count_allocations = false;
  return g_allocations;
}

} // namespace

void* operator new(std::size_t size) {
  if (g_count_allocations) {
    ++g_allocations;
  }
  if (void* ptr = std::malloc(size)) {
    return ptr;
  }
  throw std::bad_alloc();
}

void operator delete(void* ptr) noexcept {
  std::free(ptr);
}

void operator delete(void* ptr, std::size_t) noexcept {
  std::free(ptr);
}

int main() {
  OrderBook book({64, 191, 8});
  book.warm_up();
  OrderBook small({10, 12, 1});
  small.warm_up();

  reset_allocation_counter();
  if (!book.put({1, 11, Side::Ask, 100, 10}).ok() ||
      !book.put({2, 12, Side::Ask, 100, 20}).ok() ||
      !book.put({3, 13, Side::Ask, 128, 30}).ok() ||
      !book.put({4, 14, Side::Bid, 90, 40}).ok()) {
    return 1;
  }
  if (book.put({1, 99, Side::Bid, 91, 1}).status !=
      PutStatus::DuplicateOrderId) {
    return 2;
  }
  if (book.put({5, 15, Side::Bid, 200, 1}).status !=
      PutStatus::PriceOutOfRange) {
    return 3;
  }
  if (book.put({5, 15, Side::Bid, 90, 0}).status !=
      PutStatus::InvalidQuantity) {
    return 4;
  }

  auto best_ask = book.select_best_opposite(Side::Bid);
  if (!best_ask || best_ask->id != 1 || best_ask->remaining != 10) {
    return 5;
  }
  book.decrement_selected(4);
  best_ask = book.select_best_opposite(Side::Bid);
  if (!best_ask || best_ask->id != 1 || best_ask->remaining != 6) {
    return 6;
  }
  book.decrement_selected(6);
  best_ask = book.select_best_opposite(Side::Bid);
  if (!best_ask || best_ask->id != 2 || best_ask->remaining != 20) {
    return 7;
  }

  if (!book.change(2, {5}).ok()) {
    return 8;
  }
  best_ask = book.select_best_opposite(Side::Bid);
  if (!best_ask || best_ask->id != 2 || best_ask->remaining != 5) {
    return 9;
  }

  if (!book.cancel(2).ok()) {
    return 10;
  }
  best_ask = book.select_best_opposite(Side::Bid);
  if (!best_ask || best_ask->id != 3 || best_ask->price != 128) {
    return 11;
  }
  if (!book.cancel(3).ok() || book.best_ask().has_value()) {
    return 12;
  }
  if (!book.cancel(4).ok() || !book.empty()) {
    return 13;
  }

  if (!small.put({100, 1, Side::Bid, 10, 1}).ok() ||
      small.put({101, 1, Side::Bid, 11, 1}).status != PutStatus::PoolExhausted) {
    return 14;
  }

  if (stop_allocation_counter() != 0) {
    return 15;
  }

  if (!expect(book.validate_invariants()) ||
      !expect(small.validate_invariants())) {
    return 16;
  }

  return 0;
}
