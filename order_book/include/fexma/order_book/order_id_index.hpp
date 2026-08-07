/**
 * @file order_id_index.hpp
 * @brief Fixed-capacity OrderId -> OrderIndex index.
 *
 * The index uses open addressing over one preallocated bucket array. Deletion
 * uses backward-shift compaction so long-running churn does not accumulate
 * tombstones or require runtime rehash/allocation.
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
  std::size_t probes{};
  std::size_t max_probe{};
};

/**
 * @brief Fixed-size hash index from external OrderId to internal OrderIndex.
 *
 * @warning Not thread-safe and contains no atomics.
 * @note Capacity is fixed at construction; no runtime rehash is performed.
 */
class OrderIdIndex {
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
    tombstones_ = 0;
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
      // A truly empty bucket terminates a linear-probe chain. Deleted buckets
      // cannot terminate lookup because matching keys may be further ahead.
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

    std::size_t first_tombstone = bucket_count_;
    std::size_t pos = hash(id) & (bucket_count_ - 1);
    for (std::size_t probe = 0; probe < bucket_count_; ++probe) {
      record_probe(stats, probe + 1);
      Bucket& bucket = buckets_[pos];
      if (bucket.state == State::Occupied) {
        if (bucket.id == id) {
          return IndexInsertStatus::Duplicate;
        }
      } else if (bucket.state == State::Deleted) {
        if (first_tombstone == bucket_count_) {
          first_tombstone = pos;
        }
      } else {
        Bucket& target =
            first_tombstone == bucket_count_ ? bucket : buckets_[first_tombstone];
        if (target.state == State::Deleted) {
          --tombstones_;
        }
        target.id = id;
        target.slot = slot;
        target.state = State::Occupied;
        ++size_;
        return IndexInsertStatus::Ok;
      }
      pos = (pos + 1) & (bucket_count_ - 1);
    }

    if (first_tombstone != bucket_count_) {
      Bucket& target = buckets_[first_tombstone];
      target.id = id;
      target.slot = slot;
      target.state = State::Occupied;
      ++size_;
      --tombstones_;
      return IndexInsertStatus::Ok;
    }
    return IndexInsertStatus::Full;
  }

  [[nodiscard]] bool erase(OrderId id) noexcept {
    return erase(id, nullptr);
  }

  [[nodiscard]] bool erase(OrderId id, IndexProbeStats* stats) noexcept {
    if (bucket_count_ == 0) {
      return false;
    }

    std::size_t pos = hash(id) & (bucket_count_ - 1);
    for (std::size_t probe = 0; probe < bucket_count_; ++probe) {
      record_probe(stats, probe + 1);
      Bucket& bucket = buckets_[pos];
      if (bucket.state == State::Empty) {
        return false;
      }
      if (bucket.state == State::Occupied && bucket.id == id) {
        --size_;
        erase_at(pos);
        return true;
      }
      pos = (pos + 1) & (bucket_count_ - 1);
    }
    return false;
  }

  [[nodiscard]] std::size_t size() const noexcept {
    return size_;
  }

  [[nodiscard]] std::size_t bucket_count() const noexcept {
    return bucket_count_;
  }

  [[nodiscard]] std::size_t tombstone_count() const noexcept {
    return tombstones_;
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
    Occupied,
    Deleted
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
    ++stats->probes;
    if (probes > stats->max_probe) {
      stats->max_probe = probes;
    }
  }

  void erase_at(std::size_t erased_pos) noexcept {
    buckets_[erased_pos] = Bucket{};
    std::size_t candidate_pos = (erased_pos + 1) & (bucket_count_ - 1);
    while (buckets_[candidate_pos].state == State::Occupied) {
      const Bucket saved = buckets_[candidate_pos];
      buckets_[candidate_pos] = Bucket{};
      --size_;
      (void)insert(saved.id, saved.slot);
      candidate_pos = (candidate_pos + 1) & (bucket_count_ - 1);
    }
  }

  std::size_t bucket_count_{};
  std::unique_ptr<Bucket[]> buckets_;
  std::size_t size_{};
  std::size_t tombstones_{};
};

} // namespace fexma::order_book
