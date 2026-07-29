#include <fexma/order_book/order_pool.hpp>

#include <vector>

using namespace fexma::order_book;

namespace {

bool check(bool condition) noexcept {
  return condition;
}

} // namespace

int main() {
  OrderPool pool(4);
  pool.warm_up();

  std::vector<OrderSlot> slots;
  for (int i = 0; i < 4; ++i) {
    const OrderSlot slot = pool.acquire();
    if (!check(slot != invalid_order_slot) || !check(pool.in_use(slot))) {
      return 1;
    }
    slots.push_back(slot);
  }

  if (!check(pool.acquire() == invalid_order_slot) ||
      !check(pool.free_count() == 0)) {
    return 2;
  }

  pool.release(slots[1]);
  if (!check(pool.free_count() == 1) || !check(!pool.in_use(slots[1]))) {
    return 3;
  }

  const OrderSlot reused = pool.acquire();
  if (!check(reused == slots[1]) || !check(pool.free_count() == 0)) {
    return 4;
  }

  for (OrderSlot slot : slots) {
    if (slot != slots[1]) {
      pool.release(slot);
    }
  }
  pool.release(reused);

  if (!check(pool.free_count() == pool.capacity())) {
    return 5;
  }

  std::vector<bool> seen(pool.capacity(), false);
  for (int i = 0; i < 4; ++i) {
    const OrderSlot slot = pool.acquire();
    if (!check(slot < pool.capacity()) || seen[slot]) {
      return 6;
    }
    seen[slot] = true;
  }

  return 0;
}
