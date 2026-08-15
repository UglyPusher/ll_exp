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
  read_.store(0, std::memory_order_relaxed);
  durable_.store(0, std::memory_order_relaxed);
  write_.store(0, std::memory_order_relaxed);
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

PushResult Wal::try_push(std::span<const std::byte> payload) noexcept {
  const PushStatus status = check_push(payload);
  if (status != PushStatus::Ok) {
    return {status, 0};
  }

  const auto block = try_acquire_writable_block();
  if (!block) {
    return {PushStatus::Full, 0};
  }

  block->fill(payload);
  publish(*block);
  return {PushStatus::Ok, block->sequence()};
}

DurabilityResult Wal::advance_durable(std::uint32_t max_records) noexcept {
  const DurabilityStatus status = check_durability();
  if (status != DurabilityStatus::Ok) {
    return {status, durable_cursor(), 0};
  }

  const PositionRange pending = pending_range(max_records);
  if (pending.empty()) {
    return {DurabilityStatus::Ok, pending.begin, 0};
  }

  if (!persist(pending)) {
    return fail_durability(pending);
  }

  publish_durable(pending);
  return {DurabilityStatus::Ok, pending.end, pending.size()};
}

PopResult Wal::try_pop(std::span<std::byte> payload) noexcept {
  const PopStatus status = check_pop(payload);
  if (status != PopStatus::Ok) {
    return {status, 0};
  }

  const auto block = try_acquire_readable_block();
  if (!block) {
    return {PopStatus::Empty, 0};
  }

  block->copy_to(payload);
  release(*block);
  return {PopStatus::Ok, block->sequence()};
}

CloseResult Wal::close() noexcept {
  if (!open_.load(std::memory_order_acquire)) {
    return {CloseStatus::AlreadyClosed};
  }
  if (write_.load(std::memory_order_acquire) !=
      durable_.load(std::memory_order_acquire)) {
    return {CloseStatus::PendingDurability};
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

std::uint64_t Wal::read_cursor() const noexcept {
  return read_.load(std::memory_order_acquire);
}

std::uint64_t Wal::durable_cursor() const noexcept {
  return durable_.load(std::memory_order_acquire);
}

std::uint64_t Wal::write_cursor() const noexcept {
  return write_.load(std::memory_order_acquire);
}

void Wal::WritableBlock::fill(
    std::span<const std::byte> payload) const noexcept {
  std::memcpy(bytes.data(), payload.data(), payload.size());
}

std::uint64_t Wal::WritableBlock::sequence() const noexcept {
  return position + 1u;
}

void Wal::ReadableBlock::copy_to(std::span<std::byte> payload) const noexcept {
  std::memcpy(payload.data(), bytes.data(), payload.size());
}

std::uint64_t Wal::ReadableBlock::sequence() const noexcept {
  return position + 1u;
}

bool Wal::PositionRange::empty() const noexcept { return begin == end; }

std::uint32_t Wal::PositionRange::size() const noexcept {
  return static_cast<std::uint32_t>(end - begin);
}

PushStatus Wal::check_push(std::span<const std::byte> payload) const noexcept {
  if (!open_.load(std::memory_order_acquire)) {
    return PushStatus::Closed;
  }
  if (payload.size() != config_.payload_size) {
    return PushStatus::InvalidPayloadSize;
  }
  if (io_failed_.load(std::memory_order_acquire)) {
    return PushStatus::IoError;
  }
  return PushStatus::Ok;
}

std::optional<Wal::WritableBlock> Wal::try_acquire_writable_block() noexcept {
  const std::uint64_t write = write_.load(std::memory_order_relaxed);
  const std::uint64_t read = read_.load(std::memory_order_acquire);
  if ((write - read) == config_.capacity) {
    return std::nullopt;
  }

  return WritableBlock{
      write, std::span<std::byte>{slot(write), config_.payload_size}};
}

void Wal::publish(const WritableBlock& block) noexcept {
  write_.store(block.position + 1u, std::memory_order_release);
}

DurabilityStatus Wal::check_durability() const noexcept {
  if (!open_.load(std::memory_order_acquire)) {
    return DurabilityStatus::Closed;
  }
  if (io_failed_.load(std::memory_order_relaxed)) {
    return DurabilityStatus::IoError;
  }
  return DurabilityStatus::Ok;
}

Wal::PositionRange
Wal::pending_range(std::uint32_t max_records) const noexcept {
  const std::uint64_t durable = durable_.load(std::memory_order_relaxed);
  const std::uint64_t write = write_.load(std::memory_order_acquire);
  const std::uint64_t available = write - durable;
  const std::uint64_t count = std::min<std::uint64_t>(available, max_records);
  return {durable, durable + count};
}

bool Wal::persist(PositionRange range) noexcept {
  for (std::uint64_t position = range.begin; position < range.end; ++position) {
    if (!write_record(position + 1u, slot(position))) {
      return false;
    }
  }

  stream_.flush();
  return static_cast<bool>(stream_);
}

DurabilityResult Wal::fail_durability(PositionRange range) noexcept {
  io_failed_.store(true, std::memory_order_release);
  return {DurabilityStatus::IoError, range.begin, 0};
}

void Wal::publish_durable(PositionRange range) noexcept {
  durable_.store(range.end, std::memory_order_release);
}

PopStatus Wal::check_pop(std::span<std::byte> payload) const noexcept {
  if (!open_.load(std::memory_order_acquire)) {
    return PopStatus::Closed;
  }
  if (payload.size() != config_.payload_size) {
    return PopStatus::InvalidPayloadSize;
  }
  return PopStatus::Ok;
}

std::optional<Wal::ReadableBlock> Wal::try_acquire_readable_block() noexcept {
  const std::uint64_t read = read_.load(std::memory_order_relaxed);
  const std::uint64_t durable = durable_.load(std::memory_order_acquire);
  if (read == durable) {
    return std::nullopt;
  }

  return ReadableBlock{
      read, std::span<const std::byte>{slot(read), config_.payload_size}};
}

void Wal::release(const ReadableBlock& block) noexcept {
  read_.store(block.position + 1u, std::memory_order_release);
}

std::byte* Wal::slot(std::uint64_t position) noexcept {
  const std::size_t index =
      static_cast<std::size_t>(position % config_.capacity);
  return storage_ + (index * slot_stride_);
}

const std::byte* Wal::slot(std::uint64_t position) const noexcept {
  const std::size_t index =
      static_cast<std::size_t>(position % config_.capacity);
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
    const std::uint32_t chunk = std::min(
        remaining, static_cast<std::uint32_t>(zeros.size()));
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
