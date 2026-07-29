/**
 * @file test_order_book.cpp
 * @brief OrderBook API tests for atomicity, selection protocol, invariants,
 * and runtime allocation guards.
 */
#include <fexma/order_book/order_book.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <new>
#include <vector>

#ifdef _MSC_VER
#include <malloc.h>
#endif

using namespace fexma::order_book;

namespace {

inline std::uint64_t g_allocations = 0;
inline bool g_count_allocations = false;

struct Snapshot {
  std::vector<BestOrderView> bids;
  std::vector<BestOrderView> asks;
  std::vector<std::uint64_t> masks;
  std::vector<std::uint32_t> counts;
  std::vector<Quantity> quantities;
  std::optional<PriceTick> best_bid;
  std::optional<PriceTick> best_ask;
  OrderSlot free_count{};
  OrderSlot selected{};
  std::uint64_t generation{};

  [[nodiscard]] bool operator==(const Snapshot& rhs) const {
    const auto equal_views = [](const auto& lhs, const auto& rhs) {
      if (lhs.size() != rhs.size()) {
        return false;
      }
      for (std::size_t i = 0; i < lhs.size(); ++i) {
        if (lhs[i].id != rhs[i].id || lhs[i].owner_id != rhs[i].owner_id ||
            lhs[i].price != rhs[i].price ||
            lhs[i].remaining != rhs[i].remaining) {
          return false;
        }
      }
      return true;
    };
    return equal_views(bids, rhs.bids) && equal_views(asks, rhs.asks) &&
           masks == rhs.masks && counts == rhs.counts &&
           quantities == rhs.quantities && best_bid == rhs.best_bid &&
           best_ask == rhs.best_ask && free_count == rhs.free_count &&
           selected == rhs.selected && generation == rhs.generation;
  }
};

Snapshot snapshot(OrderBook& book) {
  Snapshot snap;
  book.for_each_order_for_test(Side::Bid, [&snap](BestOrderView view) {
    snap.bids.push_back(view);
    return true;
  });
  book.for_each_order_for_test(Side::Ask, [&snap](BestOrderView view) {
    snap.asks.push_back(view);
    return true;
  });
  for (std::size_t i = 0; i < book.segment_count(); ++i) {
    const PriceSegment& bid = book.segment_for_test(Side::Bid, i);
    const PriceSegment& ask = book.segment_for_test(Side::Ask, i);
    snap.masks.push_back(bid.active_mask);
    snap.masks.push_back(ask.active_mask);
    for (std::uint32_t offset = 0; offset < prices_per_segment; ++offset) {
      snap.counts.push_back(bid.levels[offset].order_count);
      snap.counts.push_back(ask.levels[offset].order_count);
      snap.quantities.push_back(bid.levels[offset].total_quantity);
      snap.quantities.push_back(ask.levels[offset].total_quantity);
    }
  }
  snap.best_bid = book.best_bid();
  snap.best_ask = book.best_ask();
  snap.free_count = book.free_count_for_test();
  snap.selected = book.selected_order_for_test();
  snap.generation = book.generation_for_test();
  return snap;
}

bool unchanged_after_failed_put(OrderBook& book, const RestingOrderData& order,
                                PutStatus expected) {
  const Snapshot before = snapshot(book);
  const PutResult result = book.put(order);
  const Snapshot after = snapshot(book);
  return result.status == expected && before == after &&
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
  OrderBook book({64, 191, 8});
  book.warm_up();
  OrderBook small({10, 12, 1});
  small.warm_up();

  if (!book.put({1, 11, Side::Ask, 100, 10}).ok() ||
      !book.put({2, 12, Side::Ask, 100, 20}).ok() ||
      !book.put({3, 13, Side::Ask, 128, 30}).ok() ||
      !book.put({4, 14, Side::Bid, 90, 40}).ok()) {
    return 1;
  }

  if (!unchanged_after_failed_put(book, {1, 99, Side::Bid, 91, 1},
                                  PutStatus::DuplicateOrderId) ||
      !unchanged_after_failed_put(book, {5, 15, Side::Bid, 200, 1},
                                  PutStatus::PriceOutOfRange) ||
      !unchanged_after_failed_put(book, {5, 15, Side::Bid, 90, 0},
                                  PutStatus::InvalidQuantity)) {
    return 2;
  }

  auto selected = book.select_best_opposite(Side::Bid);
  if (!selected || selected->id != 1) {
    return 3;
  }
  const Snapshot before_cancel_miss = snapshot(book);
  if (book.cancel(999).status != CancelStatus::NotFound ||
      !(before_cancel_miss == snapshot(book))) {
    return 4;
  }
  const Snapshot before_change_invalid = snapshot(book);
  if (book.change(1, {10}).status != ChangeStatus::InvalidQuantity ||
      !(before_change_invalid == snapshot(book))) {
    return 5;
  }
  if (book.change(1, {11}).status != ChangeStatus::InvalidQuantity ||
      !(before_change_invalid == snapshot(book))) {
    return 6;
  }

  selected = book.select_best_opposite(Side::Bid);
  book.decrement_selected(4);
  selected = book.select_best_opposite(Side::Bid);
  if (!selected || selected->id != 1 || selected->remaining != 6) {
    return 7;
  }
  book.decrement_selected(6);
  selected = book.select_best_opposite(Side::Bid);
  if (!selected || selected->id != 2 || selected->remaining != 20) {
    return 8;
  }

  if (!book.change(2, {5}).ok()) {
    return 9;
  }
  selected = book.select_best_opposite(Side::Bid);
  if (!selected || selected->id != 2 || selected->remaining != 5) {
    return 10;
  }

  OrderBook cancel_equiv_a({64, 191, 4});
  OrderBook cancel_equiv_b({64, 191, 4});
  cancel_equiv_a.warm_up();
  cancel_equiv_b.warm_up();
  (void)cancel_equiv_a.put({100, 1, Side::Ask, 100, 10});
  (void)cancel_equiv_a.put({101, 1, Side::Ask, 100, 10});
  (void)cancel_equiv_b.put({100, 1, Side::Ask, 100, 10});
  (void)cancel_equiv_b.put({101, 1, Side::Ask, 100, 10});
  if (!cancel_equiv_a.cancel(100).ok() ||
      !cancel_equiv_b.change(100, {0}).ok() ||
      !(snapshot(cancel_equiv_a) == snapshot(cancel_equiv_b))) {
    return 11;
  }

  if (!book.cancel(2).ok()) {
    return 12;
  }
  selected = book.select_best_opposite(Side::Bid);
  if (!selected || selected->id != 3 || selected->price != 128) {
    return 13;
  }
  if (!book.cancel(3).ok() || book.best_ask().has_value()) {
    return 14;
  }
  if (!book.cancel(4).ok() || !book.empty()) {
    return 15;
  }

  selected = cancel_equiv_a.select_best_opposite(Side::Bid);
  const auto first_selection = cancel_equiv_a.selected_order_for_test();
  selected = cancel_equiv_a.select_best_opposite(Side::Bid);
  if (!selected || cancel_equiv_a.selected_order_for_test() != first_selection) {
    return 16;
  }
  (void)cancel_equiv_a.put({102, 1, Side::Ask, 110, 1});
  if (cancel_equiv_a.selected_order_for_test() != invalid_order_slot) {
    return 17;
  }
  selected = cancel_equiv_a.select_best_opposite(Side::Bid);
  (void)cancel_equiv_a.cancel(102);
  if (cancel_equiv_a.selected_order_for_test() != invalid_order_slot) {
    return 18;
  }
  selected = cancel_equiv_a.select_best_opposite(Side::Bid);
  (void)cancel_equiv_a.change(101, {1});
  if (cancel_equiv_a.selected_order_for_test() != invalid_order_slot) {
    return 19;
  }

  if (!small.put({100, 1, Side::Bid, 10, 1}).ok() ||
      !unchanged_after_failed_put(small, {101, 1, Side::Bid, 11, 1},
                                  PutStatus::PoolExhausted)) {
    return 20;
  }

  OrderBook alloc_book({1, 128, 16});
  alloc_book.warm_up();
  (void)alloc_book.put({1, 1, Side::Ask, 10, 10});
  (void)alloc_book.put({2, 1, Side::Bid, 9, 10});
  reset_allocation_counter();
  (void)alloc_book.put({3, 1, Side::Ask, 10, 10});
  if (stop_allocation_counter() != 0) {
    return 21;
  }
  reset_allocation_counter();
  selected = alloc_book.select_best_opposite(Side::Bid);
  if (stop_allocation_counter() != 0 || !selected) {
    return 22;
  }
  reset_allocation_counter();
  alloc_book.decrement_selected(1);
  if (stop_allocation_counter() != 0) {
    return 23;
  }
  reset_allocation_counter();
  (void)alloc_book.cancel(2);
  if (stop_allocation_counter() != 0) {
    return 24;
  }
  reset_allocation_counter();
  (void)alloc_book.change(1, {1});
  if (stop_allocation_counter() != 0) {
    return 25;
  }

  if (!book.validate_invariants() || !small.validate_invariants() ||
      !alloc_book.validate_invariants()) {
    return 26;
  }

  if (alloc_book.pool_warm_up_stats().bytes == 0 ||
      alloc_book.index_warm_up_stats().bytes == 0 ||
      alloc_book.bid_warm_up_stats().bytes == 0 ||
      alloc_book.ask_warm_up_stats().bytes == 0) {
    return 27;
  }

  OrderBook empty({1, 2, 1});
  empty.warm_up();
  if (empty.select_best_opposite(Side::Bid).has_value() ||
      empty.selected_order_for_test() != invalid_order_slot) {
    return 28;
  }

  return 0;
}
