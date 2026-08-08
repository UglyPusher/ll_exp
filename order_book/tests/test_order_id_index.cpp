/**
 * @file test_order_id_index.cpp
 * @brief Autonomous tests for fixed OrderIdIndex probing, deletion, and churn.
 */
#include <fexma/order_book/order_id_index.hpp>

#include "order_id_index_test_access.hpp"

#include <algorithm>
#include <vector>

using namespace fexma::order_book;

namespace {

std::vector<OrderId> ids_for_home_bucket(const OrderIdIndex& index,
                                         std::size_t home_bucket,
                                         std::size_t count) {
  std::vector<OrderId> ids;
  for (OrderId id = 1; ids.size() < count; ++id) {
    if (OrderIdIndexTestAccess::home_bucket(index, id) == home_bucket) {
      ids.push_back(id);
    }
  }
  return ids;
}

} // namespace

int main() {
  OrderIdIndex index(4);

  if (index.find(10) != invalid_order_index) {
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
  if (!index.erase(20) || index.find(20) != invalid_order_index) {
    return 5;
  }
  if (index.insert(40, 2) != IndexInsertStatus::Ok ||
      index.find(40) != 2) {
    return 6;
  }

  OrderIdIndex small(1);
  std::uint32_t inserted = 0;
  for (OrderId id = 1; id < 100; ++id) {
    const IndexInsertStatus status = small.insert(id, static_cast<OrderIndex>(id));
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

  OrderIdIndex churn(64);
  std::vector<OrderId> active;
  for (OrderId id = 1; id <= 32; ++id) {
    IndexProbeStats stats{};
    if (churn.insert(id * 129, static_cast<OrderIndex>(id), &stats) !=
            IndexInsertStatus::Ok ||
        stats.probes == 0) {
      return 9;
    }
    active.push_back(id * 129);
  }

  std::size_t max_probe = 0;
  std::uint64_t total_probe = 0;
  std::uint64_t probe_ops = 0;
  for (OrderId cycle = 0; cycle < 200000; ++cycle) {
    const std::size_t victim = static_cast<std::size_t>(cycle % active.size());
    IndexProbeStats erase_stats{};
    if (!churn.erase(active[victim], &erase_stats)) {
      return 10;
    }
    total_probe += erase_stats.probes;
    max_probe = (std::max)(max_probe, erase_stats.max_probe);
    ++probe_ops;

    const OrderId next = 1000000 + cycle * 129;
    IndexProbeStats insert_stats{};
    if (churn.insert(next, static_cast<OrderIndex>(victim), &insert_stats) !=
        IndexInsertStatus::Ok) {
      return 11;
    }
    total_probe += insert_stats.probes;
    max_probe = (std::max)(max_probe, insert_stats.max_probe);
    ++probe_ops;
    active[victim] = next;

    if ((cycle % 10000) == 0) {
      for (OrderId id : active) {
        IndexProbeStats find_stats{};
        if (churn.find(id, &find_stats) == invalid_order_index ||
            find_stats.probes == 0) {
          return 12;
        }
      }
      if (max_probe > churn.bucket_count()) {
        return 13;
      }
    }
  }

  if (probe_ops == 0 || total_probe == 0) {
    return 14;
  }

  OrderIdIndex wrap_around(4);
  const std::size_t last_bucket = wrap_around.bucket_count() - 1U;
  const std::vector<OrderId> colliding_ids =
      ids_for_home_bucket(wrap_around, last_bucket, 4);
  for (std::size_t i = 0; i < colliding_ids.size(); ++i) {
    if (wrap_around.insert(colliding_ids[i], static_cast<OrderIndex>(i + 1U)) !=
        IndexInsertStatus::Ok) {
      return 15;
    }
  }
  if (!wrap_around.erase(colliding_ids[0])) {
    return 16;
  }
  for (std::size_t i = 1; i < colliding_ids.size(); ++i) {
    if (wrap_around.find(colliding_ids[i]) !=
        static_cast<OrderIndex>(i + 1U)) {
      return 17;
    }
  }
  if (!wrap_around.erase(colliding_ids[2]) ||
      wrap_around.find(colliding_ids[1]) != 2 ||
      wrap_around.find(colliding_ids[3]) != 4 ||
      wrap_around.insert(colliding_ids[0], 9) != IndexInsertStatus::Ok ||
      wrap_around.find(colliding_ids[0]) != 9) {
    return 18;
  }

  return 0;
}
