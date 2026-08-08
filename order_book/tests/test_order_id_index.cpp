/**
 * @file test_order_id_index.cpp
 * @brief Deterministic and differential tests for fixed OrderIdIndex probing.
 */
#include <fexma/order_book/order_id_index.hpp>

#include "order_id_index_test_access.hpp"

#include <algorithm>
#include <cstdint>
#include <random>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace fexma::order_book;

namespace {

using Entry = std::pair<OrderId, OrderIndex>;

[[nodiscard]] std::vector<OrderId>
ids_for_home_bucket(const OrderIdIndex& index, std::size_t home_bucket,
                    std::size_t count, OrderId first_id = 1) {
  std::vector<OrderId> ids;
  for (OrderId id = first_id; ids.size() < count; ++id) {
    if (OrderIdIndexTestAccess::home_bucket(index, id) == home_bucket) {
      ids.push_back(id);
    }
  }
  return ids;
}

[[nodiscard]] std::vector<Entry> entries(const OrderIdIndex& index) {
  std::vector<Entry> result;
  (void)index.for_each([&result](OrderId id, OrderIndex slot) {
    result.emplace_back(id, slot);
    return true;
  });
  std::sort(result.begin(), result.end());
  return result;
}

[[nodiscard]] bool valid(const OrderIdIndex& index) {
  return OrderIdIndexTestAccess::validate_invariants(index);
}

[[nodiscard]] bool erase_single_bucket() {
  OrderIdIndex index(4);
  return index.insert(10, 7) == IndexInsertStatus::Ok && valid(index) &&
         index.erase_and_get(10) == 7 && index.size() == 0 && valid(index) &&
         index.find(10) == invalid_order_index;
}

[[nodiscard]] bool insert_duplicate_and_full_table_erase() {
  OrderIdIndex index(1);
  std::vector<OrderId> ids;
  for (OrderId id = 1; ids.size() < index.bucket_count(); ++id) {
    if (index.insert(id, static_cast<OrderIndex>(id)) !=
        IndexInsertStatus::Ok) {
      return false;
    }
    ids.push_back(id);
    if (ids.size() == 1U &&
        index.insert(id, 999) != IndexInsertStatus::Duplicate) {
      return false;
    }
    if (!valid(index)) {
      return false;
    }
  }
  if (index.insert(999999, 999) != IndexInsertStatus::Full) {
    return false;
  }
  const std::size_t before_size = index.size();
  if (index.erase_and_get(ids[ids.size() / 2U]) == invalid_order_index ||
      index.size() + 1U != before_size || !valid(index)) {
    return false;
  }
  for (const OrderId id : ids) {
    if (id != ids[ids.size() / 2U] && index.find(id) == invalid_order_index) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] bool erase_cluster_position(std::size_t erased_offset) {
  OrderIdIndex index(8);
  const std::vector<OrderId> ids = ids_for_home_bucket(index, 3, 6);
  for (std::size_t i = 0; i < ids.size(); ++i) {
    if (index.insert(ids[i], static_cast<OrderIndex>(100U + i)) !=
            IndexInsertStatus::Ok ||
        !valid(index)) {
      return false;
    }
  }

  IndexProbeStats stats{};
  if (index.erase_and_get(ids[erased_offset], &stats) !=
          static_cast<OrderIndex>(100U + erased_offset) ||
      index.size() != ids.size() - 1U || !valid(index)) {
    return false;
  }
  for (std::size_t i = 0; i < ids.size(); ++i) {
    const OrderIndex expected =
        i == erased_offset ? invalid_order_index
                           : static_cast<OrderIndex>(100U + i);
    if (index.find(ids[i]) != expected) {
      return false;
    }
  }
  return stats.lookup_probes == erased_offset + 1U;
}

[[nodiscard]] bool consecutive_cluster_erases_and_refill() {
  OrderIdIndex index(16);
  const std::vector<OrderId> ids = ids_for_home_bucket(index, 5, 10);
  for (std::size_t i = 0; i < ids.size(); ++i) {
    if (index.insert(ids[i], static_cast<OrderIndex>(i + 1U)) !=
            IndexInsertStatus::Ok ||
        !valid(index)) {
      return false;
    }
  }
  for (const std::size_t erased : {std::size_t{4}, std::size_t{0},
                                   std::size_t{8}, std::size_t{2}}) {
    const std::size_t before_size = index.size();
    if (index.erase_and_get(ids[erased]) !=
            static_cast<OrderIndex>(erased + 1U) ||
        index.size() + 1U != before_size || !valid(index)) {
      return false;
    }
  }

  for (const std::size_t erased : {std::size_t{4}, std::size_t{0},
                                   std::size_t{8}, std::size_t{2}}) {
    if (index.insert(ids[erased], static_cast<OrderIndex>(erased + 1U)) !=
            IndexInsertStatus::Ok ||
        !valid(index)) {
      return false;
    }
  }
  for (std::size_t i = 0; i < ids.size(); ++i) {
    if (index.find(ids[i]) != static_cast<OrderIndex>(i + 1U)) {
      return false;
    }
  }
  return index.size() == ids.size();
}

[[nodiscard]] bool mixed_home_positions_skip_ineligible_bucket() {
  OrderIdIndex index(8);
  const OrderId home_zero = ids_for_home_bucket(index, 0, 1)[0];
  const OrderId home_one = ids_for_home_bucket(index, 1, 1)[0];
  const OrderId second_home_zero =
      ids_for_home_bucket(index, 0, 1, home_zero + 1U)[0];

  if (index.insert(home_zero, 10) != IndexInsertStatus::Ok ||
      index.insert(home_one, 11) != IndexInsertStatus::Ok ||
      index.insert(second_home_zero, 12) != IndexInsertStatus::Ok) {
    return false;
  }
  const std::size_t home_one_position =
      OrderIdIndexTestAccess::bucket_position(index, home_one);
  if (index.erase_and_get(home_zero) != 10 || !valid(index)) {
    return false;
  }

  return OrderIdIndexTestAccess::bucket_position(index, home_one) ==
             home_one_position &&
         OrderIdIndexTestAccess::bucket_position(index, second_home_zero) == 0 &&
         index.find(home_one) == 11 && index.find(second_home_zero) == 12;
}

[[nodiscard]] bool wrap_around_cluster_repair() {
  OrderIdIndex index(8);
  const std::size_t last_bucket = index.bucket_count() - 1U;
  const std::vector<OrderId> ids =
      ids_for_home_bucket(index, last_bucket, 8);
  for (std::size_t i = 0; i < ids.size(); ++i) {
    if (index.insert(ids[i], static_cast<OrderIndex>(i + 20U)) !=
            IndexInsertStatus::Ok ||
        !valid(index)) {
      return false;
    }
  }
  for (const std::size_t erased : {std::size_t{0}, std::size_t{4},
                                   std::size_t{7}}) {
    if (index.erase_and_get(ids[erased]) !=
            static_cast<OrderIndex>(erased + 20U) ||
        !valid(index)) {
      return false;
    }
  }
  for (std::size_t i = 0; i < ids.size(); ++i) {
    const bool was_erased = i == 0 || i == 4 || i == 7;
    if (index.find(ids[i]) !=
        (was_erased ? invalid_order_index
                    : static_cast<OrderIndex>(i + 20U))) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] bool missing_erase_is_atomic() {
  OrderIdIndex index(8);
  for (OrderId id = 1; id <= 6; ++id) {
    if (index.insert(id * 17U, static_cast<OrderIndex>(id)) !=
        IndexInsertStatus::Ok) {
      return false;
    }
  }
  const std::vector<Entry> before = entries(index);
  const std::size_t before_size = index.size();
  IndexProbeStats stats{};
  return index.erase_and_get(999999, &stats) == invalid_order_index &&
         index.size() == before_size && entries(index) == before &&
         stats.repair_buckets_scanned == 0 && valid(index);
}

[[nodiscard]] bool randomized_differential() {
  OrderIdIndex index(64);
  std::unordered_map<OrderId, OrderIndex> model;
  std::mt19937_64 random(0x5EED1234ULL);

  for (std::size_t step = 0; step < 100000; ++step) {
    const OrderId id = 1U + random() % 500U;
    const int operation = static_cast<int>(random() % 100U);
    if (operation < 45 && model.size() < 100U) {
      const OrderIndex slot = static_cast<OrderIndex>(1U + random() % 100000U);
      const bool duplicate = model.contains(id);
      const IndexInsertStatus status = index.insert(id, slot);
      if (status != (duplicate ? IndexInsertStatus::Duplicate
                               : IndexInsertStatus::Ok)) {
        return false;
      }
      if (!duplicate) {
        model.emplace(id, slot);
      }
      if (!valid(index)) {
        return false;
      }
    } else if (operation < 80) {
      const auto found = model.find(id);
      const OrderIndex expected =
          found == model.end() ? invalid_order_index : found->second;
      if (index.erase_and_get(id) != expected) {
        return false;
      }
      if (found != model.end()) {
        model.erase(found);
      }
      if (!valid(index)) {
        return false;
      }
    } else {
      const auto found = model.find(id);
      const OrderIndex expected =
          found == model.end() ? invalid_order_index : found->second;
      if (index.find(id) != expected) {
        return false;
      }
    }

    if (index.size() != model.size()) {
      return false;
    }
    if ((step % 1000U) == 0) {
      for (const auto& [active_id, slot] : model) {
        if (index.find(active_id) != slot) {
          return false;
        }
      }
    }
  }
  return true;
}

} // namespace

int main() {
  if (!erase_single_bucket()) {
    return 1;
  }
  if (!insert_duplicate_and_full_table_erase()) {
    return 2;
  }
  if (!erase_cluster_position(0) || !erase_cluster_position(3) ||
      !erase_cluster_position(5)) {
    return 3;
  }
  if (!consecutive_cluster_erases_and_refill()) {
    return 4;
  }
  if (!mixed_home_positions_skip_ineligible_bucket()) {
    return 5;
  }
  if (!wrap_around_cluster_repair()) {
    return 6;
  }
  if (!missing_erase_is_atomic()) {
    return 7;
  }
  if (!randomized_differential()) {
    return 8;
  }
  return 0;
}
