/**
 * @file wal.cpp
 * @brief Bounded WAL frontier ring with a physical durability stage.
 */

#include <fexma/wal/wal.hpp>

#include "physical_wal_file.hpp"

#include <algorithm>
#include <cstring>
#include <new>

namespace fexma::wal {
namespace {

[[nodiscard]] std::size_t aligned_payload_size(const WalConfig& config) noexcept {
  return ((static_cast<std::size_t>(config.payload_size) + config.alignment - 1u) /
          config.alignment) *
         config.alignment;
}

} // namespace

bool valid_config(const WalConfig& config) noexcept {
  if (config.payload_size == 0 || config.capacity == 0 ||
      config.alignment < alignof(void*)) {
    return false;
  }
  return (config.alignment & (config.alignment - 1u)) == 0;
}

Wal::Storage::~Storage() { release(); }

OpenStatus Wal::Storage::initialize(const WalConfig& config) noexcept {
  const std::size_t stride = aligned_payload_size(config);
  if (stride > (std::numeric_limits<std::size_t>::max() / config.capacity)) {
    return OpenStatus::InvalidConfig;
  }

  alignment_ = config.alignment;
  size_ = stride * config.capacity;
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

  file_.reset(new (std::nothrow) detail::PhysicalWalFile{});
  if (!file_) {
    storage_.release();
    return {OpenStatus::AllocationFailed};
  }
  if (!file_->create(path, config)) {
    release_resources();
    return {OpenStatus::IoError};
  }

  config_ = config;
  tail_frontier_.value.store(0, std::memory_order_relaxed);
  durable_frontier_.value.store(0, std::memory_order_relaxed);
  head_frontier_.value.store(0, std::memory_order_relaxed);
  tail_slot_ = 0;
  durable_slot_ = 0;
  head_slot_ = 0;
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
  if (io_failed_.load(std::memory_order_acquire)) {
    return {PublishStatus::IoError, 0};
  }

  const std::uint64_t head = head_frontier_.value.load(std::memory_order_relaxed);
  const std::uint64_t tail = tail_frontier_.value.load(std::memory_order_acquire);
  if ((head - tail) == config_.capacity) {
    return {PublishStatus::Full, 0};
  }

  std::span<std::byte> block = storage_.block_at_slot(head_slot_);
  std::memcpy(block.data(), payload.data(), payload.size());
  head_slot_ = storage_.next_slot(head_slot_);
  head_frontier_.value.store(head + 1u, std::memory_order_release);
  return {PublishStatus::Ok, head + 1u};
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
    if (!file_->append_record(position + 1u,
                              storage_.block_at_slot(slot))) {
      io_failed_.store(true, std::memory_order_release);
      return {DurabilityStatus::IoError, durable, 0};
    }
    slot = storage_.next_slot(slot);
  }

  if (!file_->sync()) {
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
  std::memcpy(payload.data(), block.data(), payload.size());
  tail_slot_ = storage_.next_slot(tail_slot_);
  tail_frontier_.value.store(tail + 1u, std::memory_order_release);
  return {ConsumeStatus::Ok, tail + 1u};
}

CloseResult Wal::close() noexcept {
  if (!open_.load(std::memory_order_acquire)) {
    return {CloseStatus::AlreadyClosed};
  }
  const bool io_failed = io_failed_.load(std::memory_order_acquire);
  if (!io_failed &&
      head_frontier_.value.load(std::memory_order_acquire) !=
          durable_frontier_.value.load(std::memory_order_acquire)) {
    return {CloseStatus::PendingDurability};
  }

  open_.store(false, std::memory_order_release);
  const bool close_ok = file_->close();
  file_.reset();
  storage_.release();
  return {!io_failed && close_ok ? CloseStatus::Ok : CloseStatus::IoError};
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
  if (file_) {
    (void)file_->close();
    file_.reset();
  }
  storage_.release();
}

} // namespace fexma::wal
