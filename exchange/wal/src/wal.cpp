/**
 * @file wal.cpp
 * @brief Implements the fixed-payload durable queue.
 */

#include <fexma/wal/wal.hpp>

#include <algorithm>
#include <array>
#include <cstring>
#include <new>

namespace fexma::wal {
namespace {

template <class T>
[[nodiscard]] std::span<const std::byte> as_bytes(const T& value) noexcept {
  return {reinterpret_cast<const std::byte*>(&value), sizeof(T)};
}

[[nodiscard]] bool write_exact(std::ofstream& stream,
                               std::span<const std::byte> bytes) noexcept {
  stream.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
  return static_cast<bool>(stream);
}

[[nodiscard]] std::uint32_t padding_size(const WalConfig& config) noexcept {
  const std::uint64_t raw_size = sizeof(RecordHeader) + config.payload_size;
  return static_cast<std::uint32_t>(aligned_record_size(config) - raw_size);
}

[[nodiscard]] std::size_t aligned_payload_size(const WalConfig& config) noexcept {
  return ((static_cast<std::size_t>(config.payload_size) + config.alignment - 1u) /
          config.alignment) *
         config.alignment;
}

} // namespace

std::uint32_t crc32_bytes(const void* data, std::size_t size) noexcept {
  const auto* bytes = static_cast<const std::uint8_t*>(data);
  std::uint32_t crc = 0xFFFFFFFFu;

  for (std::size_t i = 0; i < size; ++i) {
    crc ^= bytes[i];
    for (int bit = 0; bit < 8; ++bit) {
      const std::uint32_t mask = 0u - (crc & 1u);
      crc = (crc >> 1u) ^ (0xEDB88320u & mask);
    }
  }

  return ~crc;
}

std::uint32_t file_header_crc32(FileHeader header) noexcept {
  header.header_crc32 = 0;
  return crc32_bytes(&header, sizeof(header));
}

std::uint32_t record_header_crc32(RecordHeader header) noexcept {
  header.header_crc32 = 0;
  return crc32_bytes(&header, sizeof(header));
}

std::uint64_t aligned_record_size(const WalConfig& config) noexcept {
  const std::uint64_t raw_size = sizeof(RecordHeader) + config.payload_size;
  const std::uint64_t alignment = config.alignment;
  return ((raw_size + alignment - 1u) / alignment) * alignment;
}

bool valid_config(const WalConfig& config) noexcept {
  if (config.payload_size == 0 || config.capacity == 0 ||
      config.alignment < alignof(void*)) {
    return false;
  }
  return (config.alignment & (config.alignment - 1u)) == 0;
}

Wal::~Wal() {
  open_.store(false, std::memory_order_relaxed);
  if (stream_.is_open()) {
    stream_.close();
  }
  release_storage();
}

OpenResult Wal::open(const std::filesystem::path& path,
                     const WalConfig& config) noexcept {
  if (open_.load(std::memory_order_relaxed)) {
    return {OpenStatus::AlreadyOpen};
  }
  if (!valid_config(config)) {
    return {OpenStatus::InvalidConfig};
  }

  config_ = config;

  const std::size_t stride = aligned_payload_size(config);
  if (stride > (std::numeric_limits<std::size_t>::max() / config.capacity)) {
    return {OpenStatus::InvalidConfig};
  }

  const std::size_t storage_size = stride * config.capacity;
  storage_ = static_cast<std::byte*>(::operator new(
      storage_size, std::align_val_t{config.alignment}, std::nothrow));
  if (storage_ == nullptr) {
    return {OpenStatus::AllocationFailed};
  }

  stream_.open(path, std::ios::binary | std::ios::trunc);
  if (!stream_.is_open()) {
    release_storage();
    return {OpenStatus::IoError};
  }

  slot_stride_ = stride;
  accepted_sequence_.store(0, std::memory_order_relaxed);
  durable_sequence_.store(0, std::memory_order_relaxed);
  released_sequence_.store(0, std::memory_order_relaxed);
  io_failed_.store(false, std::memory_order_relaxed);

  FileHeader header{};
  header.payload_size = config.payload_size;
  header.alignment = config.alignment;
  header.header_crc32 = file_header_crc32(header);

  if (!write_exact(stream_, as_bytes(header))) {
    stream_.close();
    release_storage();
    return {OpenStatus::IoError};
  }

  stream_.flush();
  if (!stream_) {
    stream_.close();
    release_storage();
    return {OpenStatus::IoError};
  }
  open_.store(true, std::memory_order_release);
  return {OpenStatus::Ok};
}

EnqueueResult Wal::try_enqueue(std::span<const std::byte> payload) noexcept {
  if (!open_.load(std::memory_order_acquire)) {
    return {EnqueueStatus::Closed, 0};
  }
  if (payload.size() != config_.payload_size) {
    return {EnqueueStatus::InvalidPayloadSize, 0};
  }
  if (io_failed_.load(std::memory_order_acquire)) {
    return {EnqueueStatus::IoError, 0};
  }

  const std::uint64_t accepted =
      accepted_sequence_.load(std::memory_order_relaxed);
  const std::uint64_t released =
      released_sequence_.load(std::memory_order_acquire);
  if ((accepted - released) == config_.capacity) {
    return {EnqueueStatus::Full, 0};
  }

  const std::uint64_t sequence = accepted + 1u;
  std::memcpy(slot(sequence), payload.data(), payload.size());
  accepted_sequence_.store(sequence, std::memory_order_release);
  return {EnqueueStatus::Ok, sequence};
}

DurabilityResult Wal::make_durable(std::uint32_t max_records) noexcept {
  if (!open_.load(std::memory_order_acquire)) {
    return {DurabilityStatus::Closed, durable_sequence(), 0};
  }
  if (io_failed_.load(std::memory_order_relaxed)) {
    return {DurabilityStatus::IoError, durable_sequence(), 0};
  }

  const std::uint64_t durable =
      durable_sequence_.load(std::memory_order_relaxed);
  const std::uint64_t accepted =
      accepted_sequence_.load(std::memory_order_acquire);
  const std::uint64_t available = accepted - durable;
  const std::uint64_t count = std::min<std::uint64_t>(available, max_records);
  if (count == 0) {
    return {DurabilityStatus::Ok, durable, 0};
  }

  const std::uint64_t target = durable + count;
  for (std::uint64_t sequence = durable + 1u; sequence <= target; ++sequence) {
    if (!write_record(sequence, slot(sequence))) {
      io_failed_.store(true, std::memory_order_release);
      return {DurabilityStatus::IoError, durable, 0};
    }
  }

  stream_.flush();
  if (!stream_) {
    io_failed_.store(true, std::memory_order_release);
    return {DurabilityStatus::IoError, durable, 0};
  }

  durable_sequence_.store(target, std::memory_order_release);
  return {DurabilityStatus::Ok, target, static_cast<std::uint32_t>(count)};
}

DequeueResult Wal::try_dequeue(std::span<std::byte> payload) noexcept {
  if (!open_.load(std::memory_order_acquire)) {
    return {DequeueStatus::Closed, 0};
  }
  if (payload.size() != config_.payload_size) {
    return {DequeueStatus::InvalidPayloadSize, 0};
  }

  const std::uint64_t released =
      released_sequence_.load(std::memory_order_relaxed);
  const std::uint64_t durable =
      durable_sequence_.load(std::memory_order_acquire);
  if (released == durable) {
    return {DequeueStatus::Empty, 0};
  }

  const std::uint64_t sequence = released + 1u;
  std::memcpy(payload.data(), slot(sequence), payload.size());
  released_sequence_.store(sequence, std::memory_order_release);
  return {DequeueStatus::Ok, sequence};
}

CloseResult Wal::close() noexcept {
  if (!open_.load(std::memory_order_acquire)) {
    return {CloseStatus::AlreadyClosed};
  }
  if (accepted_sequence_.load(std::memory_order_acquire) !=
      durable_sequence_.load(std::memory_order_acquire)) {
    return {CloseStatus::PendingAccepted};
  }

  open_.store(false, std::memory_order_release);
  stream_.close();
  const bool close_ok = !stream_.fail();
  release_storage();
  return {close_ok ? CloseStatus::Ok : CloseStatus::IoError};
}

bool Wal::is_open() const noexcept {
  return open_.load(std::memory_order_acquire);
}

const WalConfig& Wal::config() const noexcept { return config_; }

std::uint64_t Wal::accepted_sequence() const noexcept {
  return accepted_sequence_.load(std::memory_order_acquire);
}

std::uint64_t Wal::durable_sequence() const noexcept {
  return durable_sequence_.load(std::memory_order_acquire);
}

std::byte* Wal::slot(std::uint64_t sequence) noexcept {
  const std::size_t index = static_cast<std::size_t>((sequence - 1u) % config_.capacity);
  return storage_ + (index * slot_stride_);
}

const std::byte* Wal::slot(std::uint64_t sequence) const noexcept {
  const std::size_t index = static_cast<std::size_t>((sequence - 1u) % config_.capacity);
  return storage_ + (index * slot_stride_);
}

bool Wal::write_record(std::uint64_t sequence,
                       const std::byte* payload) noexcept {
  RecordHeader header{};
  header.sequence = sequence;
  header.payload_crc32 = crc32_bytes(payload, config_.payload_size);
  header.header_crc32 = record_header_crc32(header);

  if (!write_exact(stream_, as_bytes(header)) ||
      !write_exact(stream_, {payload, config_.payload_size})) {
    return false;
  }

  std::uint32_t remaining = padding_size(config_);
  const std::array<std::byte, default_alignment> zeros{};
  while (remaining != 0) {
    const std::uint32_t chunk =
        std::min<std::uint32_t>(remaining, zeros.size());
    if (!write_exact(stream_, {zeros.data(), chunk})) {
      return false;
    }
    remaining -= chunk;
  }
  return true;
}

void Wal::release_storage() noexcept {
  if (storage_ != nullptr) {
    ::operator delete(storage_, std::align_val_t{config_.alignment});
    storage_ = nullptr;
  }
  slot_stride_ = 0;
}

} // namespace fexma::wal
