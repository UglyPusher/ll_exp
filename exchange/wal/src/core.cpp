/**
 * @file core.cpp
 * @brief Bounded in-memory WAL string implementation.
 */

#include <fexma/wal/core.hpp>

#include <cstring>
#include <limits>
#include <new>

namespace fexma::wal {
namespace {

[[nodiscard]] bool checked_add(std::size_t left, std::size_t right,
                               std::size_t& out) noexcept {
  if (left > std::numeric_limits<std::size_t>::max() - right) return false;
  out = left + right;
  return true;
}

[[nodiscard]] bool checked_mul(std::size_t left, std::size_t right,
                               std::size_t& out) noexcept {
  if (left != 0 && right > std::numeric_limits<std::size_t>::max() / left) {
    return false;
  }
  out = left * right;
  return true;
}

[[nodiscard]] bool checked_align_up(std::size_t value, std::size_t alignment,
                                    std::size_t& out) noexcept {
  std::size_t biased{};
  if (!checked_add(value, alignment - 1u, biased)) return false;
  out = (biased / alignment) * alignment;
  return true;
}

[[nodiscard]] bool valid_runtime_config(
    const WalRuntimeConfig& config) noexcept {
  if (config.payload_size == 0 || config.capacity == 0 ||
      config.alignment < alignof(void*) || config.first_sequence == 0 ||
      (config.alignment & (config.alignment - 1u)) != 0) {
    return false;
  }
  std::size_t stride{};
  std::size_t size{};
  return checked_align_up(config.payload_size, config.alignment, stride) &&
         checked_mul(stride, config.capacity, size);
}

} // namespace

WalCore::Storage::~Storage() { release(); }

OpenStatus WalCore::Storage::initialize(
    const WalRuntimeConfig& config) noexcept {
  std::size_t stride{};
  std::size_t size{};
  if (!checked_align_up(config.payload_size, config.alignment, stride) ||
      !checked_mul(stride, config.capacity, size)) {
    return OpenStatus::InvalidConfig;
  }

  alignment_ = config.alignment;
  size_ = size;
  data_ = static_cast<std::byte*>(
      ::operator new(size_, std::align_val_t{alignment_}, std::nothrow));
  if (data_ == nullptr) {
    size_ = 0;
    return OpenStatus::AllocationFailed;
  }
  stride_ = stride;
  payload_size_ = config.payload_size;
  capacity_ = config.capacity;
  std::memset(data_, 0, size_);
  return OpenStatus::Ok;
}

void WalCore::Storage::release() noexcept {
  if (data_ != nullptr) {
    ::operator delete(data_, std::align_val_t{alignment_});
  }
  data_ = nullptr;
  size_ = 0;
  stride_ = 0;
  payload_size_ = 0;
  capacity_ = 0;
}

std::span<std::byte>
WalCore::Storage::block_at_slot(std::uint32_t slot) noexcept {
  return {data_ + static_cast<std::size_t>(slot) * stride_, payload_size_};
}

std::span<const std::byte>
WalCore::Storage::block_at_slot(std::uint32_t slot) const noexcept {
  return {data_ + static_cast<std::size_t>(slot) * stride_, payload_size_};
}

std::uint32_t WalCore::Storage::next_slot(std::uint32_t slot) const noexcept {
  ++slot;
  return slot == capacity_ ? 0 : slot;
}

WalCore::~WalCore() { close(); }

OpenResult WalCore::open(const WalRuntimeConfig& config) noexcept {
  if (is_open()) return {OpenStatus::AlreadyOpen};
  if (!valid_runtime_config(config)) return {OpenStatus::InvalidConfig};

  const OpenStatus storage_status = storage_.initialize(config);
  if (storage_status != OpenStatus::Ok) return {storage_status};

  config_ = config;
  head_slot_ = 0;
  tail_frontier_.value.store(0, std::memory_order_relaxed);
  head_frontier_.value.store(0, std::memory_order_relaxed);
  sequence_exhausted_.store(false, std::memory_order_relaxed);
  open_.store(true, std::memory_order_release);
  return {OpenStatus::Ok};
}

PublishResult
WalCore::try_publish(std::span<const std::byte> payload) noexcept {
  if (!is_open()) return {PublishStatus::Closed, 0};
  if (payload.size() != config_.payload_size) {
    return {PublishStatus::InvalidPayloadSize, 0};
  }
  if (sequence_exhausted()) return {PublishStatus::SequenceExhausted, 0};

  const Position head = head_frontier_.value.load(std::memory_order_relaxed);
  const Position tail = tail_frontier_.value.load(std::memory_order_acquire);
  if (head - tail == config_.capacity) return {PublishStatus::Full, 0};
  if (head > std::numeric_limits<std::uint64_t>::max() -
                 config_.first_sequence) {
    sequence_exhausted_.store(true, std::memory_order_release);
    return {PublishStatus::SequenceExhausted, 0};
  }

  std::span<std::byte> block = storage_.block_at_slot(head_slot_);
  std::memcpy(block.data(), payload.data(), payload.size());
  head_slot_ = storage_.next_slot(head_slot_);
  head_frontier_.value.store(head + 1u, std::memory_order_release);
  return {PublishStatus::Ok, config_.first_sequence + head};
}

AccessResult WalCore::try_view(Position position) const noexcept {
  if (!is_open()) return {ViewStatus::Closed};

  const Position tail = tail_frontier_.value.load(std::memory_order_acquire);
  if (position < tail) return {ViewStatus::Reclaimed};
  const Position head = head_frontier_.value.load(std::memory_order_acquire);
  if (position >= head) return {ViewStatus::Unpublished};

  const auto slot = static_cast<std::uint32_t>(position % config_.capacity);
  return {ViewStatus::Ok,
          {position, config_.first_sequence + position,
           storage_.block_at_slot(slot)}};
}

ReclaimStatus WalCore::reclaim(Position end) noexcept {
  if (!is_open()) return ReclaimStatus::Closed;
  const Position tail = tail_frontier_.value.load(std::memory_order_relaxed);
  const Position head = head_frontier_.value.load(std::memory_order_acquire);
  if (end < tail || end > head) return ReclaimStatus::InvalidPosition;
  tail_frontier_.value.store(end, std::memory_order_release);
  return ReclaimStatus::Ok;
}

void WalCore::close() noexcept {
  open_.store(false, std::memory_order_release);
  storage_.release();
}

bool WalCore::is_open() const noexcept {
  return open_.load(std::memory_order_acquire);
}

bool WalCore::sequence_exhausted() const noexcept {
  return sequence_exhausted_.load(std::memory_order_acquire);
}

Position WalCore::head() const noexcept {
  return head_frontier_.value.load(std::memory_order_acquire);
}

Position WalCore::tail() const noexcept {
  return tail_frontier_.value.load(std::memory_order_acquire);
}

const WalRuntimeConfig& WalCore::config() const noexcept { return config_; }

} // namespace fexma::wal
