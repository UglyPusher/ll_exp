/**
 * @file wal.cpp
 * @brief Bounded WAL frontier ring with a physical durability stage.
 */

#include <fexma/wal/wal.hpp>

#include "physical_wal_adapter.hpp"

#include <algorithm>
#include <cstring>
#include <new>

namespace fexma::wal {
namespace {

[[nodiscard]] bool checked_add(std::size_t left, std::size_t right,
                               std::size_t& out) noexcept {
  if (left > std::numeric_limits<std::size_t>::max() - right) {
    return false;
  }
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
  if (!checked_add(value, alignment - 1u, biased)) {
    return false;
  }
  out = (biased / alignment) * alignment;
  return true;
}

[[nodiscard]] bool valid_stream_kind(StreamKind kind) noexcept {
  switch (kind) {
  case StreamKind::Generic:
  case StreamKind::Command:
  case StreamKind::Event:
    return true;
  }
  return false;
}

[[nodiscard]] bool sequence_at_position(const WalConfig& config,
                                        std::uint64_t position,
                                        std::uint64_t& sequence) noexcept {
  if (position >
      std::numeric_limits<std::uint64_t>::max() - config.first_sequence) {
    return false;
  }
  sequence = config.first_sequence + position;
  return true;
}

} // namespace

Wal::Wal() = default;

bool valid_config(const WalConfig& config) noexcept {
  if (config.payload_size == 0 || config.capacity == 0 ||
      config.alignment < alignof(void*) || config.first_sequence == 0 ||
      !valid_stream_kind(config.stream_kind)) {
    return false;
  }
  if (config.stream_kind != StreamKind::Generic &&
      (config.stream_id == 0 || config.epoch_id == 0 ||
       config.manifest_id == 0)) {
    return false;
  }
  if ((config.alignment & (config.alignment - 1u)) != 0) {
    return false;
  }

  std::size_t storage_stride{};
  std::size_t storage_size{};
  if (!checked_align_up(config.payload_size, config.alignment,
                        storage_stride) ||
      !checked_mul(storage_stride, config.capacity, storage_size)) {
    return false;
  }
  return records_offset(config) != 0 && aligned_record_size(config) != 0;
}

Wal::Storage::~Storage() { release(); }

OpenStatus Wal::Storage::initialize(const WalConfig& config) noexcept {
  std::size_t stride{};
  std::size_t size{};
  if (!checked_align_up(config.payload_size, config.alignment, stride) ||
      !checked_mul(stride, config.capacity, size)) {
    return OpenStatus::InvalidConfig;
  }

  alignment_ = config.alignment;
  size_ = size;
  data_ = static_cast<std::byte*>(::operator new(
      size_, std::align_val_t{alignment_}, std::nothrow));
  if (data_ == nullptr) {
    size_ = 0;
    return OpenStatus::AllocationFailed;
  }

  stride_ = stride;
  payload_size_ = config.payload_size;
  capacity_ = config.capacity;

  // Commit and initialize every page before any role enters its hot path.
  std::memset(data_, 0, size_);
  return OpenStatus::Ok;
}

void Wal::Storage::release() noexcept {
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
Wal::Storage::block_at_slot(std::uint32_t slot) noexcept {
  return {data_ + (static_cast<std::size_t>(slot) * stride_), payload_size_};
}

std::span<const std::byte>
Wal::Storage::block_at_slot(std::uint32_t slot) const noexcept {
  return {data_ + (static_cast<std::size_t>(slot) * stride_), payload_size_};
}

std::uint32_t Wal::Storage::next_slot(std::uint32_t slot) const noexcept {
  ++slot;
  return slot == capacity_ ? 0 : slot;
}

Wal::~Wal() {
  open_.store(false, std::memory_order_relaxed);
  release_resources();
}

OpenResult Wal::open(const std::filesystem::path& path,
                     const WalConfig& config) noexcept {
  if (open_.load(std::memory_order_relaxed)) {
    return {OpenStatus::AlreadyOpen};
  }
  if (!valid_config(config)) {
    return {OpenStatus::InvalidConfig};
  }

  const OpenStatus storage_status = storage_.initialize(config);
  if (storage_status != OpenStatus::Ok) {
    return {storage_status};
  }

  physical_wal_.reset(new (std::nothrow) detail::PhysicalWalAdapter{});
  if (!physical_wal_) {
    storage_.release();
    return {OpenStatus::AllocationFailed};
  }
  const OpenStatus file_status = physical_wal_->create(path, config);
  if (file_status != OpenStatus::Ok) {
    release_resources();
    return {file_status};
  }

  config_ = config;
  tail_frontier_.value.store(0, std::memory_order_relaxed);
  durable_frontier_.value.store(0, std::memory_order_relaxed);
  head_frontier_.value.store(0, std::memory_order_relaxed);
  tail_slot_ = 0;
  durable_slot_ = 0;
  head_slot_ = 0;
  sequence_exhausted_.store(false, std::memory_order_relaxed);
  io_failed_.store(false, std::memory_order_relaxed);
  open_.store(true, std::memory_order_release);
  return {OpenStatus::Ok};
}

PublishResult Wal::try_publish(std::span<const std::byte> payload) noexcept {
  if (!open_.load(std::memory_order_acquire)) {
    return {PublishStatus::Closed, 0};
  }
  if (payload.size() != config_.payload_size) {
    return {PublishStatus::InvalidPayloadSize, 0};
  }
  if (sequence_exhausted_.load(std::memory_order_acquire)) {
    return {PublishStatus::SequenceExhausted, 0};
  }
  if (io_failed_.load(std::memory_order_acquire)) {
    return {PublishStatus::IoError, 0};
  }

  const std::uint64_t head = head_frontier_.value.load(std::memory_order_relaxed);
  const std::uint64_t tail = tail_frontier_.value.load(std::memory_order_acquire);
  if ((head - tail) == config_.capacity) {
    return {PublishStatus::Full, 0};
  }

  std::uint64_t sequence{};
  if (!sequence_at_position(config_, head, sequence)) {
    sequence_exhausted_.store(true, std::memory_order_release);
    return {PublishStatus::SequenceExhausted, 0};
  }

  std::span<std::byte> block = storage_.block_at_slot(head_slot_);
  std::memcpy(block.data(), payload.data(), payload.size());
  head_slot_ = storage_.next_slot(head_slot_);
  head_frontier_.value.store(head + 1u, std::memory_order_release);
  return {PublishStatus::Ok, sequence};
}

DurabilityResult Wal::advance_durable(std::uint32_t batch_size) noexcept {
  if (!open_.load(std::memory_order_acquire)) {
    return {DurabilityStatus::Closed,
            durable_frontier_.value.load(std::memory_order_acquire), 0};
  }
  if (io_failed_.load(std::memory_order_relaxed)) {
    return {DurabilityStatus::IoError,
            durable_frontier_.value.load(std::memory_order_acquire), 0};
  }

  const std::uint64_t durable =
      durable_frontier_.value.load(std::memory_order_relaxed);
  const std::uint64_t head = head_frontier_.value.load(std::memory_order_acquire);
  const std::uint64_t available = head - durable;
  const std::uint64_t count = std::min<std::uint64_t>(available, batch_size);
  const std::uint64_t end = durable + count;

  if (count == 0) {
    return {DurabilityStatus::Ok, durable, 0};
  }

  std::uint32_t slot = durable_slot_;
  for (std::uint64_t position = durable; position < end; ++position) {
    std::uint64_t sequence{};
    if (!sequence_at_position(config_, position, sequence)) {
      sequence_exhausted_.store(true, std::memory_order_release);
      return {DurabilityStatus::SequenceExhausted, durable, 0};
    }
    if (!physical_wal_->append_record(sequence,
                                      storage_.block_at_slot(slot))) {
      io_failed_.store(true, std::memory_order_release);
      return {DurabilityStatus::IoError, durable, 0};
    }
    slot = storage_.next_slot(slot);
  }

  if (!physical_wal_->sync()) {
    io_failed_.store(true, std::memory_order_release);
    return {DurabilityStatus::IoError, durable, 0};
  }

  durable_slot_ = slot;
  durable_frontier_.value.store(end, std::memory_order_release);
  return {DurabilityStatus::Ok, end, static_cast<std::uint32_t>(count)};
}

ConsumeResult Wal::try_consume(std::span<std::byte> payload) noexcept {
  if (!open_.load(std::memory_order_acquire)) {
    return {ConsumeStatus::Closed, 0};
  }
  if (payload.size() != config_.payload_size) {
    return {ConsumeStatus::InvalidPayloadSize, 0};
  }

  const std::uint64_t tail = tail_frontier_.value.load(std::memory_order_relaxed);
  const std::uint64_t durable =
      durable_frontier_.value.load(std::memory_order_acquire);
  if (tail == durable) {
    return {ConsumeStatus::Empty, 0};
  }

  std::span<const std::byte> block = storage_.block_at_slot(tail_slot_);
  std::uint64_t sequence{};
  (void)sequence_at_position(config_, tail, sequence);
  std::memcpy(payload.data(), block.data(), payload.size());
  tail_slot_ = storage_.next_slot(tail_slot_);
  tail_frontier_.value.store(tail + 1u, std::memory_order_release);
  return {ConsumeStatus::Ok, sequence};
}

CloseResult Wal::close() noexcept {
  if (!open_.load(std::memory_order_acquire)) {
    return {CloseStatus::AlreadyClosed};
  }
  const std::uint64_t tail =
      tail_frontier_.value.load(std::memory_order_acquire);
  const std::uint64_t durable =
      durable_frontier_.value.load(std::memory_order_acquire);
  if (tail != durable) {
    return {CloseStatus::PendingConsumption};
  }

  const bool sequence_exhausted =
      sequence_exhausted_.load(std::memory_order_acquire);
  const bool io_failed = io_failed_.load(std::memory_order_acquire);
  if (!io_failed &&
      durable != head_frontier_.value.load(std::memory_order_acquire)) {
    return {CloseStatus::PendingDurability};
  }

  open_.store(false, std::memory_order_release);
  const bool close_ok = physical_wal_->close();
  physical_wal_.reset();
  storage_.release();
  if (!close_ok || io_failed) {
    return {CloseStatus::IoError};
  }
  return {sequence_exhausted ? CloseStatus::SequenceExhausted
                             : CloseStatus::Ok};
}

bool Wal::is_open() const noexcept {
  return open_.load(std::memory_order_acquire);
}

const WalConfig& Wal::config() const noexcept { return config_; }

WalSnapshot Wal::snapshot() const noexcept {
  // Monotonic frontiers loaded in ownership order preserve tail <= durable <=
  // head even while the three roles are progressing concurrently.
  const std::uint64_t tail = tail_frontier_.value.load(std::memory_order_acquire);
  const std::uint64_t durable =
      durable_frontier_.value.load(std::memory_order_acquire);
  const std::uint64_t head = head_frontier_.value.load(std::memory_order_acquire);
  return {tail, durable, head};
}

void Wal::release_resources() noexcept {
  if (physical_wal_) {
    (void)physical_wal_->close();
    physical_wal_.reset();
  }
  storage_.release();
}

} // namespace fexma::wal
