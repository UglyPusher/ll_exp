/**
 * @file order_id_index.hpp
 * @brief Fixed-capacity OrderId -> OrderIndex index.
 *
 * The index uses open addressing over one preallocated bucket array. Deletion
 * uses backward-shift repair so lookup can terminate at the first Empty bucket
 * without runtime rehash or allocation.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

#include <fexma/order_book/types.hpp>

namespace fexma::order_book {

enum class IndexInsertStatus : std::uint8_t {
  Ok,
  Duplicate,
  Full
};

/** @brief Probe diagnostics for tests and benchmarks. */
struct IndexProbeStats {
  std::size_t lookup_probes{};
  std::size_t repair_buckets_scanned{};
  std::size_t repair_buckets_moved{};
  std::size_t max_lookup_probe{};
  std::size_t max_repair_scan{};
};

/**
 * @brief Fixed-size hash index from external OrderId to internal OrderIndex.
 *
 * @warning Not thread-safe and contains no atomics.
 * @note Capacity is fixed at construction; no runtime rehash is performed.
 */
class OrderIdIndex {
  friend class OrderIdIndexTestAccess;

public:
  OrderIdIndex() = default;

  explicit OrderIdIndex(OrderCapacity capacity)
      : bucket_count_(bucket_count_for(capacity)),
        buckets_(bucket_count_ == 0 ? nullptr
                                    : std::make_unique<Bucket[]>(bucket_count_)) {
    clear();
  }

  OrderIdIndex(const OrderIdIndex&) = delete;
  OrderIdIndex& operator=(const OrderIdIndex&) = delete;
  OrderIdIndex(OrderIdIndex&&) noexcept = default;
  OrderIdIndex& operator=(OrderIdIndex&&) noexcept = default;

  void clear() noexcept {
    size_ = 0;
    for (std::size_t i = 0; i < bucket_count_; ++i) {
      buckets_[i] = Bucket{};
    }
  }

  [[nodiscard]] OrderIndex find(OrderId id) const noexcept {
    return find(id, nullptr);
  }

  [[nodiscard]] OrderIndex find(OrderId id,
                                IndexProbeStats* stats) const noexcept {
    if (bucket_count_ == 0) {
      return invalid_order_index;
    }

    std::size_t pos = hash(id) & (bucket_count_ - 1);
    for (std::size_t probe = 0; probe < bucket_count_; ++probe) {
      record_probe(stats, probe + 1);
      const Bucket& bucket = buckets_[pos];
      if (bucket.state == State::Empty) {
        return invalid_order_index;
      }
      if (bucket.state == State::Occupied && bucket.id == id) {
        return bucket.slot;
      }
      pos = (pos + 1) & (bucket_count_ - 1);
    }
    return invalid_order_index;
  }

  [[nodiscard]] IndexInsertStatus insert(OrderId id, OrderIndex slot) noexcept {
    return insert(id, slot, nullptr);
  }

  [[nodiscard]] IndexInsertStatus insert(OrderId id, OrderIndex slot,
                                         IndexProbeStats* stats) noexcept {
    if (bucket_count_ == 0) {
      return IndexInsertStatus::Full;
    }

    std::size_t pos = hash(id) & (bucket_count_ - 1);
    for (std::size_t probe = 0; probe < bucket_count_; ++probe) {
      record_probe(stats, probe + 1);
      Bucket& bucket = buckets_[pos];
      if (bucket.state == State::Occupied) {
        if (bucket.id == id) {
          return IndexInsertStatus::Duplicate;
        }
      } else {
        bucket.id = id;
        bucket.slot = slot;
        bucket.state = State::Occupied;
        ++size_;
        return IndexInsertStatus::Ok;
      }
      pos = (pos + 1) & (bucket_count_ - 1);
    }
    return IndexInsertStatus::Full;
  }

  [[nodiscard]] bool erase(OrderId id) noexcept {
    return erase_and_get(id, nullptr) != invalid_order_index;
  }

  [[nodiscard]] bool erase(OrderId id, IndexProbeStats* stats) noexcept {
    return erase_and_get(id, stats) != invalid_order_index;
  }

  [[nodiscard]] OrderIndex erase_and_get(OrderId id) noexcept {
    return erase_and_get(id, nullptr);
  }

  [[nodiscard]] OrderIndex erase_and_get(OrderId id,
                                         IndexProbeStats* stats) noexcept {
    if (bucket_count_ == 0) {
      return invalid_order_index;
    }

    std::size_t pos = hash(id) & (bucket_count_ - 1);
    for (std::size_t probe = 0; probe < bucket_count_; ++probe) {
      record_probe(stats, probe + 1);
      Bucket& bucket = buckets_[pos];
      if (bucket.state == State::Empty) {
        return invalid_order_index;
      }
      if (bucket.state == State::Occupied && bucket.id == id) {
        const OrderIndex erased_slot = bucket.slot;
        --size_;
        erase_at(pos, stats);
        return erased_slot;
      }
      pos = (pos + 1) & (bucket_count_ - 1);
    }
    return invalid_order_index;
  }

  [[nodiscard]] std::size_t size() const noexcept {
    return size_;
  }

  [[nodiscard]] std::size_t bucket_count() const noexcept {
    return bucket_count_;
  }

  [[nodiscard]] double load_factor() const noexcept {
    return bucket_count_ == 0 ? 0.0
                              : static_cast<double>(size_) /
                                    static_cast<double>(bucket_count_);
  }

  [[nodiscard]] static constexpr std::size_t bucket_size() noexcept {
    return sizeof(Bucket);
  }

  [[nodiscard]] static constexpr std::size_t bucket_align() noexcept {
    return alignof(Bucket);
  }

  [[nodiscard]] std::size_t byte_size() const noexcept {
    return sizeof(Bucket) * bucket_count_;
  }

  template <typename Fn>
  bool for_each(Fn&& fn) const noexcept {
    for (std::size_t i = 0; i < bucket_count_; ++i) {
      const Bucket& bucket = buckets_[i];
      if (bucket.state == State::Occupied && !fn(bucket.id, bucket.slot)) {
        return false;
      }
    }
    return true;
  }

private:
  enum class State : std::uint8_t {
    Empty,
    Occupied
  };

  struct Bucket {
    OrderId id{};
    OrderIndex slot{invalid_order_index};
    State state{State::Empty};
  };

  static std::size_t bucket_count_for(OrderCapacity capacity) noexcept {
    std::size_t count = 1;
    const std::size_t required =
        capacity == 0 ? 1 : static_cast<std::size_t>(capacity) * 2 + 1;
    while (count < required) {
      count <<= 1;
    }
    return count;
  }

  static std::size_t hash(OrderId id) noexcept {
    std::uint64_t x = id;
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return static_cast<std::size_t>(x);
  }

  static void record_probe(IndexProbeStats* stats, std::size_t probes) noexcept {
    if (stats == nullptr) {
      return;
    }
    ++stats->lookup_probes;
    if (probes > stats->max_lookup_probe) {
      stats->max_lookup_probe = probes;
    }
  }

  void erase_at(std::size_t erased_pos, IndexProbeStats* stats) noexcept {
    const std::size_t mask = bucket_count_ - 1U;
    std::size_t hole_pos = erased_pos;
    buckets_[hole_pos] = Bucket{};
    std::size_t candidate_pos = (erased_pos + 1U) & mask;
    std::size_t repair_scan = 0;
    while (repair_scan + 1U < bucket_count_ &&
           buckets_[candidate_pos].state == State::Occupied) {
      ++repair_scan;
      if (stats != nullptr) {
        ++stats->repair_buckets_scanned;
      }

      const std::size_t home_pos = hash(buckets_[candidate_pos].id) & mask;
      const std::size_t hole_distance = (hole_pos - home_pos) & mask;
      const std::size_t candidate_distance =
          (candidate_pos - home_pos) & mask;
      if (hole_distance < candidate_distance) {
        buckets_[hole_pos] = buckets_[candidate_pos];
        buckets_[candidate_pos] = Bucket{};
        hole_pos = candidate_pos;
        if (stats != nullptr) {
          ++stats->repair_buckets_moved;
        }
      }
      candidate_pos = (candidate_pos + 1U) & mask;
    }
    if (stats != nullptr && repair_scan > stats->max_repair_scan) {
      stats->max_repair_scan = repair_scan;
    }
  }

  std::size_t bucket_count_{};
  std::unique_ptr<Bucket[]> buckets_;
  std::size_t size_{};
};

} // namespace fexma::order_book
