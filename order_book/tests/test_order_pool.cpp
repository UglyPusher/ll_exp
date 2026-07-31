/**
 * @file test_order_pool.cpp
 * @brief Autonomous return-code tests for OrderPool acquire/release/prefault.
 */
#include <fexma/order_book/order_pool.hpp>

#include <vector>

using namespace fexma::order_book;

namespace {

bool check(bool condition) noexcept {
  return condition;
}

} // namespace

int main() {
  {
    OrderPool raw_pool(4, OrderPool::Uninitialized{});
    if (!check(raw_pool.capacity() == 4) ||
        !check(raw_pool.free_count() == 0) ||
        !check(raw_pool.free_head() == invalid_order_index) ||
        !check(raw_pool.acquire() == invalid_order_index) ||
        !check(raw_pool.validate_freelist())) {
      return 20;
    }
    raw_pool.prefault_pages();
    if (!check(raw_pool.free_count() == 0) ||
        !check(raw_pool.free_head() == invalid_order_index) ||
        !check(raw_pool.validate_freelist())) {
      return 21;
    }
    raw_pool.reset();
    if (!check(raw_pool.free_count() == raw_pool.capacity()) ||
        !check(raw_pool.free_head() == 0) ||
        !check(raw_pool.validate_freelist())) {
      return 22;
    }
  }

  OrderPool pool(4);
  pool.prefault_pages();

  std::vector<OrderIndex> slots;
  for (int i = 0; i < 4; ++i) {
    const OrderIndex slot = pool.acquire();
    if (!check(slot != invalid_order_index) || !check(pool.in_use(slot))) {
      return 1;
    }
    slots.push_back(slot);
  }

  if (!check(pool.acquire() == invalid_order_index) ||
      !check(pool.free_count() == 0) || !check(pool.validate_freelist())) {
    return 2;
  }

  pool.release(slots[1]);
  if (!check(pool.free_count() == 1) || !check(!pool.in_use(slots[1])) ||
      !check(pool.validate_freelist())) {
    return 3;
  }

  const OrderIndex reused = pool.acquire();
  if (!check(reused == slots[1]) || !check(pool.free_count() == 0) ||
      !check(pool.validate_freelist())) {
    return 4;
  }

  for (OrderIndex slot : slots) {
    if (slot != slots[1]) {
      pool.release(slot);
    }
  }
  pool.release(reused);

  if (!check(pool.free_count() == pool.capacity()) ||
      !check(pool.validate_freelist())) {
    return 5;
  }

  std::vector<bool> seen(pool.capacity(), false);
  for (int i = 0; i < 4; ++i) {
    const OrderIndex slot = pool.acquire();
    if (!check(slot < pool.capacity()) || seen[slot]) {
      return 6;
    }
    seen[slot] = true;
  }

  pool.release(slots[0]);
  const OrderCapacity free_before_double_release = pool.free_count();
  const OrderIndex head_before_double_release = pool.free_head();
  pool.release(slots[0]);
  if (!check(pool.free_count() == free_before_double_release) ||
      !check(pool.free_head() == head_before_double_release) ||
      !check(pool.validate_freelist())) {
    return 7;
  }

  pool.release(pool.capacity());
  if (!check(pool.free_count() == free_before_double_release) ||
      !check(pool.free_head() == head_before_double_release) ||
      !check(pool.validate_freelist())) {
    return 8;
  }

  {
    OrderPool prefault_pool(6);
    const OrderIndex first = prefault_pool.acquire();
    const OrderIndex second = prefault_pool.acquire();
    const OrderIndex third = prefault_pool.acquire();
    prefault_pool[first].id = 11;
    prefault_pool[first].owner_id = 101;
    prefault_pool[first].price = 1001;
    prefault_pool[first].remaining = 7;
    prefault_pool[first].prev = invalid_order_index;
    prefault_pool[first].next = third;
    prefault_pool[first].side = Side::Ask;
    prefault_pool[third].id = 33;
    prefault_pool[third].owner_id = 303;
    prefault_pool[third].price = 1003;
    prefault_pool[third].remaining = 9;
    prefault_pool[third].prev = first;
    prefault_pool[third].next = invalid_order_index;
    prefault_pool[third].side = Side::Bid;
    prefault_pool.release(second);

    const OrderIndex free_head = prefault_pool.free_head();
    const OrderCapacity free_count = prefault_pool.free_count();
    const OrderIndex released_next = prefault_pool[second].next;

    prefault_pool.prefault_pages();

    if (!check(prefault_pool.free_head() == free_head) ||
        !check(prefault_pool.free_count() == free_count) ||
        !check(prefault_pool[second].next == released_next) ||
        !check(!prefault_pool.in_use(second))) {
      return 9;
    }
    if (!check(prefault_pool.in_use(first)) ||
        !check(prefault_pool[first].id == 11) ||
        !check(prefault_pool[first].owner_id == 101) ||
        !check(prefault_pool[first].price == 1001) ||
        !check(prefault_pool[first].remaining == 7) ||
        !check(prefault_pool[first].next == third) ||
        !check(prefault_pool[first].side == Side::Ask) ||
        !check(prefault_pool.in_use(third)) ||
        !check(prefault_pool[third].id == 33) ||
        !check(prefault_pool[third].owner_id == 303) ||
        !check(prefault_pool[third].price == 1003) ||
        !check(prefault_pool[third].remaining == 9) ||
        !check(prefault_pool[third].prev == first) ||
        !check(prefault_pool[third].side == Side::Bid)) {
      return 10;
    }
  }

  {
    OrderPool reset_pool(5);
    const OrderIndex first = reset_pool.acquire();
    const OrderIndex second = reset_pool.acquire();
    reset_pool[first].id = 77;
    reset_pool[second].id = 88;
    reset_pool.release(first);
    reset_pool.reset();
    if (!check(reset_pool.free_count() == reset_pool.capacity()) ||
        !check(reset_pool.free_head() == 0) ||
        !check(reset_pool.validate_freelist())) {
      return 11;
    }
    std::vector<bool> reset_seen(reset_pool.capacity(), false);
    for (OrderCapacity i = 0; i < reset_pool.capacity(); ++i) {
      const OrderIndex slot = reset_pool.acquire();
      if (!check(slot < reset_pool.capacity()) || reset_seen[slot]) {
        return 12;
      }
      reset_seen[slot] = true;
    }
    if (!check(reset_pool.acquire() == invalid_order_index)) {
      return 13;
    }
  }

  {
    OrderPool corrupt_cycle(3);
    corrupt_cycle[0].next = 0;
    if (check(corrupt_cycle.validate_freelist())) {
      return 14;
    }
  }

  {
    OrderPool corrupt_repeat(3);
    corrupt_repeat[0].next = 1;
    corrupt_repeat[1].next = 0;
    if (check(corrupt_repeat.validate_freelist())) {
      return 15;
    }
  }

  {
    OrderPool corrupt_range(3);
    corrupt_range[0].next = corrupt_range.capacity();
    if (check(corrupt_range.validate_freelist())) {
      return 16;
    }
  }

  {
    OrderPool corrupt_count(3);
    corrupt_count[0].next = invalid_order_index;
    if (check(corrupt_count.validate_freelist())) {
      return 17;
    }
  }

  return 0;
}
