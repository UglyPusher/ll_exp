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

class OrderIdIndex {
public:
  OrderIdIndex() = default;

  explicit OrderIdIndex(std::size_t capacity)
      : bucket_count_(bucket_count_for(capacity)),
        buckets_(bucket_count_ == 0 ? nullptr
                                    : std::make_unique<Bucket[]>(bucket_count_)) {
    clear();
  }

  OrderIdIndex(const OrderIdIndex&) = delete;
  OrderIdIndex& operator=(const OrderIdIndex&) = delete;
  OrderIdIndex(OrderIdIndex&&) noexcept = default;
  OrderIdIndex& operator=(OrderIdIndex&&) noexcept = default;

  void warm_up(std::uint32_t page_size = 4096) noexcept {
    touch_pages(buckets_.get(), sizeof(Bucket) * bucket_count_, page_size);
    clear();
  }

  void clear() noexcept {
    size_ = 0;
    tombstones_ = 0;
    for (std::size_t i = 0; i < bucket_count_; ++i) {
      buckets_[i] = Bucket{};
    }
  }

  [[nodiscard]] OrderSlot find(OrderId id) const noexcept {
    if (bucket_count_ == 0) {
      return invalid_order_slot;
    }

    std::size_t pos = hash(id) & (bucket_count_ - 1);
    for (std::size_t probe = 0; probe < bucket_count_; ++probe) {
      const Bucket& bucket = buckets_[pos];
      if (bucket.state == State::Empty) {
        return invalid_order_slot;
      }
      if (bucket.state == State::Occupied && bucket.id == id) {
        return bucket.slot;
      }
      pos = (pos + 1) & (bucket_count_ - 1);
    }
    return invalid_order_slot;
  }

  [[nodiscard]] IndexInsertStatus insert(OrderId id, OrderSlot slot) noexcept {
    if (bucket_count_ == 0) {
      return IndexInsertStatus::Full;
    }

    std::size_t first_tombstone = bucket_count_;
    std::size_t pos = hash(id) & (bucket_count_ - 1);
    for (std::size_t probe = 0; probe < bucket_count_; ++probe) {
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
    if (bucket_count_ == 0) {
      return false;
    }

    std::size_t pos = hash(id) & (bucket_count_ - 1);
    for (std::size_t probe = 0; probe < bucket_count_; ++probe) {
      Bucket& bucket = buckets_[pos];
      if (bucket.state == State::Empty) {
        return false;
      }
      if (bucket.state == State::Occupied && bucket.id == id) {
        bucket.state = State::Deleted;
        bucket.slot = invalid_order_slot;
        --size_;
        ++tombstones_;
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
    OrderSlot slot{invalid_order_slot};
    State state{State::Empty};
  };

  static std::size_t bucket_count_for(std::size_t capacity) noexcept {
    std::size_t count = 1;
    const std::size_t required = capacity == 0 ? 1 : capacity * 2 + 1;
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

  static void touch_pages(void* memory, std::size_t bytes,
                          std::uint32_t page_size) noexcept {
    if (memory == nullptr || bytes == 0) {
      return;
    }
    const std::size_t step = page_size == 0 ? 4096U : page_size;
    auto* raw = static_cast<volatile std::uint8_t*>(memory);
    for (std::size_t offset = 0; offset < bytes; offset += step) {
      raw[offset] = raw[offset];
    }
    raw[bytes - 1] = raw[bytes - 1];
  }

  std::size_t bucket_count_{};
  std::unique_ptr<Bucket[]> buckets_;
  std::size_t size_{};
  std::size_t tombstones_{};
};

} // namespace fexma::order_book
