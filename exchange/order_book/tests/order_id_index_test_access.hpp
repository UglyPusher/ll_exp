/**
 * @file order_id_index_test_access.hpp
 * @brief Test-only accessors for OrderIdIndex internals.
 */
#pragma once

#include <fexma/order_book/order_id_index.hpp>

namespace fexma::order_book {

class OrderIdIndexTestAccess {
public:
  [[nodiscard]] static std::size_t
  home_bucket(const OrderIdIndex& index, OrderId id) noexcept {
    return OrderIdIndex::hash(id) & (index.bucket_count_ - 1U);
  }

  [[nodiscard]] static std::size_t bucket_position(const OrderIdIndex& index,
                                                   OrderId id) noexcept {
    for (std::size_t position = 0; position < index.bucket_count_; ++position) {
      const OrderIdIndex::Bucket& bucket = index.buckets_[position];
      if (bucket.state == OrderIdIndex::State::Occupied && bucket.id == id) {
        return position;
      }
    }
    return index.bucket_count_;
  }

  [[nodiscard]] static bool validate_invariants(
      const OrderIdIndex& index) noexcept {
    std::size_t occupied_count = 0;
    const std::size_t mask = index.bucket_count_ - 1U;
    for (std::size_t position = 0; position < index.bucket_count_; ++position) {
      const OrderIdIndex::Bucket& bucket = index.buckets_[position];
      if (bucket.state == OrderIdIndex::State::Empty) {
        continue;
      }
      ++occupied_count;

      std::size_t probe_position = OrderIdIndex::hash(bucket.id) & mask;
      std::size_t steps = 0;
      while (probe_position != position) {
        if (index.buckets_[probe_position].state ==
                OrderIdIndex::State::Empty ||
            steps++ >= index.bucket_count_) {
          return false;
        }
        probe_position = (probe_position + 1U) & mask;
      }
    }
    return occupied_count == index.size_;
  }
};

} // namespace fexma::order_book
