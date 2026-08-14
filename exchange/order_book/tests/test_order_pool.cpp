/**
 * @file test_order_pool.cpp
 * @brief Autonomous return-code tests for the internal OrderPool component.
 */
#include <fexma/order_book/detail/order_pool.hpp>

#include "order_pool_test_access.hpp"

#include <random>
#include <stdexcept>
#include <utility>
#include <vector>

using namespace fexma::order_book;

namespace {

using detail::OrderPool;
using detail::OrderPoolTestAccess;

bool check(bool condition) noexcept {
  return condition;
}

} // namespace

int main() {
  {
    OrderPool empty_pool(0);
    if (!check(OrderPoolTestAccess::capacity(empty_pool) == 0) ||
        !check(OrderPoolTestAccess::free_count(empty_pool) == 0) ||
        !check(OrderPoolTestAccess::free_head(empty_pool) ==
               invalid_order_index) ||
        !check(empty_pool.emplace(1, 1, 1, 1, Side::Bid) ==
               invalid_order_index) ||
        !check(!empty_pool.contains(0)) ||
        !check(empty_pool.validate_freelist())) {
      return 1;
    }
  }

  {
    bool rejected = false;
    try {
      OrderPool invalid_pool(invalid_order_index);
      (void)invalid_pool;
    } catch (const std::invalid_argument&) {
      rejected = true;
    }
    if (!check(rejected)) {
      return 2;
    }
  }

  {
    OrderPool pool(4);
    if (!check(OrderPoolTestAccess::capacity(pool) == 4) ||
        !check(OrderPoolTestAccess::free_count(pool) == 4) ||
        !check(OrderPoolTestAccess::free_head(pool) == 0) ||
        !check(pool.validate_freelist())) {
      return 3;
    }

    std::vector<OrderIndex> slots;
    for (OrderCapacity i = 0; i < 4; ++i) {
      const OrderIndex slot =
          pool.emplace(i + 10, i + 100, i + 1000, i + 1, Side::Ask);
      if (!check(slot != invalid_order_index) || !check(pool.contains(slot))) {
        return 4;
      }
      slots.push_back(slot);
    }

    if (!check(pool.emplace(99, 99, 99, 99, Side::Bid) ==
               invalid_order_index) ||
        !check(OrderPoolTestAccess::free_count(pool) == 0) ||
        !check(pool.validate_freelist())) {
      return 5;
    }

    const detail::Order& order = pool.get_unchecked(slots[2]);
    if (!check(order.id == 12) || !check(order.owner_id == 102) ||
        !check(order.price == 1002) || !check(order.remaining == 3) ||
        !check(order.prev == invalid_order_index) ||
        !check(order.next == invalid_order_index) ||
        !check(order.side == Side::Ask)) {
      return 6;
    }

    pool.get_unchecked(slots[2]).prev = slots[1];
    pool.get_unchecked(slots[2]).next = slots[3];
    pool.release(slots[2]);
    if (!check(!pool.contains(slots[2])) ||
        !check(pool.contains(slots[1])) ||
        !check(OrderPoolTestAccess::free_count(pool) == 1) ||
        !check(OrderPoolTestAccess::free_head(pool) == slots[2]) ||
        !check(pool.validate_freelist())) {
      return 7;
    }

    const OrderIndex reused =
        pool.emplace(77, 88, 99, 11, Side::Bid);
    const detail::Order& reused_order = pool.get_unchecked(reused);
    if (!check(reused == slots[2]) || !check(reused_order.id == 77) ||
        !check(reused_order.owner_id == 88) ||
        !check(reused_order.price == 99) ||
        !check(reused_order.remaining == 11) ||
        !check(reused_order.prev == invalid_order_index) ||
        !check(reused_order.next == invalid_order_index) ||
        !check(reused_order.side == Side::Bid) ||
        !check(pool.validate_freelist())) {
      return 8;
    }

    for (OrderIndex slot : slots) {
      if (slot != slots[2]) {
        pool.release(slot);
      }
    }
    pool.release(reused);
    if (!check(OrderPoolTestAccess::free_count(pool) == 4) ||
        !check(pool.validate_freelist())) {
      return 9;
    }

    const OrderIndex first = pool.emplace(1, 1, 1, 1, Side::Bid);
    pool.release(first);
    const OrderIndex second = pool.emplace(2, 2, 2, 2, Side::Ask);
    if (!check(first == second)) {
      return 10;
    }
  }

#ifdef NDEBUG
  {
    OrderPool pool(2);
    const OrderIndex slot = pool.emplace(1, 1, 1, 1, Side::Bid);
    pool.release(slot);
    const OrderCapacity free_count = OrderPoolTestAccess::free_count(pool);
    const OrderIndex free_head = OrderPoolTestAccess::free_head(pool);

    pool.release(slot);
    pool.release(OrderPoolTestAccess::capacity(pool));
    if (!check(OrderPoolTestAccess::free_count(pool) == free_count) ||
        !check(OrderPoolTestAccess::free_head(pool) == free_head) ||
        !check(pool.validate_freelist())) {
      return 11;
    }
  }
#endif

  {
    OrderPool source(3);
    const OrderIndex slot =
        source.emplace(91, 92, 93, 94, Side::Ask);
    OrderPool target(std::move(source));
    if (!check(OrderPoolTestAccess::capacity(source) == 0) ||
        !check(OrderPoolTestAccess::free_count(source) == 0) ||
        !check(OrderPoolTestAccess::free_head(source) == invalid_order_index) ||
        !check(!source.contains(slot)) ||
        !check(source.validate_freelist())) {
      return 12;
    }
    if (!check(OrderPoolTestAccess::capacity(target) == 3) ||
        !check(target.contains(slot)) ||
        !check(target.get_unchecked(slot).id == 91) ||
        !check(target.validate_freelist())) {
      return 13;
    }

    OrderPool assigned(1);
    assigned = std::move(target);
    if (!check(OrderPoolTestAccess::capacity(target) == 0) ||
        !check(OrderPoolTestAccess::free_count(target) == 0) ||
        !check(OrderPoolTestAccess::free_head(target) == invalid_order_index) ||
        !check(!target.contains(slot)) ||
        !check(target.validate_freelist())) {
      return 14;
    }
    if (!check(OrderPoolTestAccess::capacity(assigned) == 3) ||
        !check(assigned.contains(slot)) ||
        !check(assigned.get_unchecked(slot).id == 91) ||
        !check(assigned.validate_freelist())) {
      return 15;
    }
  }

  {
    OrderPool corrupt_cycle(3);
    OrderPoolTestAccess::set_next(corrupt_cycle, 0, 0);
    if (check(corrupt_cycle.validate_freelist())) {
      return 16;
    }
  }

  {
    OrderPool corrupt_range(3);
    OrderPoolTestAccess::set_next(corrupt_range, 0, 3);
    if (check(corrupt_range.validate_freelist())) {
      return 17;
    }
  }

  {
    OrderPool corrupt_count(3);
    OrderPoolTestAccess::set_free_count(corrupt_count, 2);
    if (check(corrupt_count.validate_freelist())) {
      return 18;
    }
  }

  {
    OrderPool lost_slot(3);
    const OrderIndex slot = lost_slot.emplace(1, 1, 1, 1, Side::Bid);
    OrderPoolTestAccess::set_in_use(lost_slot, slot, false);
    if (check(lost_slot.validate_freelist())) {
      return 19;
    }
  }

  {
    constexpr OrderCapacity model_capacity = 16;
    OrderPool model_pool(model_capacity);
    std::vector<bool> model(model_capacity, false);
    std::mt19937 rng(0x5eed);
    for (int step = 0; step < 2000; ++step) {
      const int op = static_cast<int>(rng() % 10);
      if (op < 6) {
        const OrderIndex slot =
            model_pool.emplace(static_cast<OrderId>(step), 1, 1, 1, Side::Bid);
        OrderCapacity active = 0;
        for (bool used : model) {
          active += used ? 1U : 0U;
        }
        if (active == model_capacity) {
          if (!check(slot == invalid_order_index)) {
            return 20;
          }
        } else {
          if (!check(slot < model_capacity) || !check(!model[slot])) {
            return 21;
          }
          model[slot] = true;
        }
      } else {
        const OrderIndex slot = static_cast<OrderIndex>(rng() % model_capacity);
        if (model[slot]) {
          model_pool.release(slot);
          model[slot] = false;
        }
      }

      OrderCapacity active = 0;
      for (OrderIndex slot = 0; slot < model_capacity; ++slot) {
        active += model[slot] ? 1U : 0U;
        if (!check(model_pool.contains(slot) == model[slot])) {
          return 22;
        }
      }
      if (!check(OrderPoolTestAccess::free_count(model_pool) + active ==
                 model_capacity) ||
          !check(model_pool.validate_freelist())) {
        return 23;
      }
    }
  }

  return 0;
}
