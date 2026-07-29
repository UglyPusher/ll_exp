#include <fexma/order_book/order_id_index.hpp>

using namespace fexma::order_book;

int main() {
  OrderIdIndex index(4);
  index.warm_up();

  if (index.find(10) != invalid_order_slot) {
    return 1;
  }
  if (index.insert(10, 1) != IndexInsertStatus::Ok ||
      index.insert(20, 2) != IndexInsertStatus::Ok ||
      index.insert(30, 3) != IndexInsertStatus::Ok) {
    return 2;
  }
  if (index.insert(10, 4) != IndexInsertStatus::Duplicate) {
    return 3;
  }
  if (index.find(20) != 2) {
    return 4;
  }
  if (!index.erase(20) || index.find(20) != invalid_order_slot) {
    return 5;
  }
  if (index.insert(40, 2) != IndexInsertStatus::Ok ||
      index.find(40) != 2) {
    return 6;
  }

  OrderIdIndex small(1);
  std::uint32_t inserted = 0;
  for (OrderId id = 1; id < 100; ++id) {
    const IndexInsertStatus status = small.insert(id, static_cast<OrderSlot>(id));
    if (status == IndexInsertStatus::Full) {
      break;
    }
    if (status != IndexInsertStatus::Ok) {
      return 7;
    }
    ++inserted;
  }
  if (inserted == 0 || small.insert(1000, 1) != IndexInsertStatus::Full) {
    return 8;
  }

  return 0;
}
