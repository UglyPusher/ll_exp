#include "physical_wal_file.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <limits>

#if defined(_WIN32)
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace fexma::wal {
namespace {

void put_u16_le(std::span<std::byte> out, std::size_t offset,
                std::uint16_t value) noexcept {
  out[offset] = static_cast<std::byte>(value & 0xffu);
  out[offset + 1u] = static_cast<std::byte>((value >> 8u) & 0xffu);
}

void put_u32_le(std::span<std::byte> out, std::size_t offset,
                std::uint32_t value) noexcept {
  out[offset] = static_cast<std::byte>(value & 0xffu);
  out[offset + 1u] = static_cast<std::byte>((value >> 8u) & 0xffu);
  out[offset + 2u] = static_cast<std::byte>((value >> 16u) & 0xffu);
  out[offset + 3u] = static_cast<std::byte>((value >> 24u) & 0xffu);
}

void put_u64_le(std::span<std::byte> out, std::size_t offset,
                std::uint64_t value) noexcept {
  for (std::size_t byte = 0; byte < 8u; ++byte) {
    out[offset + byte] =
        static_cast<std::byte>((value >> (byte * 8u)) & 0xffu);
  }
}

[[nodiscard]] bool checked_add(std::uint64_t left, std::uint64_t right,
                               std::uint64_t& out) noexcept {
  if (left > std::numeric_limits<std::uint64_t>::max() - right) {
    return false;
  }
  out = left + right;
  return true;
}

[[nodiscard]] bool checked_align_up(std::uint64_t value, std::uint64_t alignment,
                                    std::uint64_t& out) noexcept {
  std::uint64_t biased{};
  if (!checked_add(value, alignment - 1u, biased)) {
    return false;
  }
  out = (biased / alignment) * alignment;
  return true;
}

[[nodiscard]] std::uint32_t padding_size(const WalConfig& config) noexcept {
  const std::uint64_t raw_size =
      physical_record_header_size + config.payload_size;
  return static_cast<std::uint32_t>(aligned_record_size(config) - raw_size);
}

[[nodiscard]] std::uint32_t file_header_padding_size(
    const WalConfig& config) noexcept {
  return records_offset(config) - physical_file_header_size;
}

#if !defined(_WIN32)
[[nodiscard]] bool
sync_parent_directory(const std::filesystem::path& path) noexcept {
  try {
    std::filesystem::path parent = path.parent_path();
    if (parent.empty()) {
      parent = ".";
    }
    const int descriptor = ::open(parent.c_str(), O_RDONLY | O_DIRECTORY);
    if (descriptor == -1) {
      return false;
    }
    const bool synced = ::fsync(descriptor) == 0;
    const bool closed = ::close(descriptor) == 0;
    return synced && closed;
  } catch (...) {
    return false;
  }
}
#endif

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
  const auto bytes = serialize_file_header(header);
  return crc32_bytes(bytes.data(), bytes.size());
}

std::uint32_t record_header_crc32(RecordHeader header) noexcept {
  header.header_crc32 = 0;
  const auto bytes = serialize_record_header(header);
  return crc32_bytes(bytes.data(), bytes.size());
}

std::uint64_t aligned_record_size(const WalConfig& config) noexcept {
  std::uint64_t raw_size{};
  if (!checked_add(physical_record_header_size, config.payload_size, raw_size)) {
    return 0;
  }
  std::uint64_t stride{};
  if (!checked_align_up(raw_size, config.alignment, stride)) {
    return 0;
  }
  return stride;
}

std::uint32_t records_offset(const WalConfig& config) noexcept {
  std::uint64_t offset{};
  if (!checked_align_up(physical_file_header_size, config.alignment, offset) ||
      offset > std::numeric_limits<std::uint32_t>::max()) {
    return 0;
  }
  return static_cast<std::uint32_t>(offset);
}

std::array<std::byte, physical_file_header_size>
serialize_file_header(FileHeader header) noexcept {
  std::array<std::byte, physical_file_header_size> out{};
  put_u32_le(out, 0, header.magic);
  put_u16_le(out, 4, header.version);
  put_u16_le(out, 6, header.header_size);
  put_u32_le(out, 8, header.payload_size);
  put_u32_le(out, 12, header.alignment);
  put_u64_le(out, 16, header.next_sequence);
  put_u32_le(out, 24, header.header_crc32);
  put_u32_le(out, 28, header.records_offset);
  put_u32_le(out, 32, header.payload_schema_version);
  return out;
}

std::array<std::byte, physical_record_header_size>
serialize_record_header(RecordHeader header) noexcept {
  std::array<std::byte, physical_record_header_size> out{};
  put_u32_le(out, 0, header.magic);
  put_u16_le(out, 4, header.version);
  put_u16_le(out, 6, header.header_size);
  put_u64_le(out, 8, header.sequence);
  put_u32_le(out, 16, header.payload_crc32);
  put_u32_le(out, 20, header.header_crc32);
  return out;
}

namespace detail {
namespace {

PhysicalWalFileTestControl* test_control{};

[[nodiscard]] bool inject_append_failure() noexcept {
  if (test_control == nullptr) {
    return false;
  }
  const std::uint64_t call = test_control->append_calls++;
  return call == test_control->fail_append_call;
}

[[nodiscard]] bool inject_sync_failure() noexcept {
  if (test_control == nullptr) {
    return false;
  }
  const std::uint64_t call = test_control->sync_calls++;
  return call == test_control->fail_sync_call;
}

} // namespace

PhysicalWalAdapter::~PhysicalWalAdapter() { (void)close(); }

OpenStatus PhysicalWalAdapter::create(const std::filesystem::path& path,
                                      const WalConfig& config) noexcept {
  if (is_open()) {
    return OpenStatus::IoError;
  }

#if defined(_WIN32)
  const HANDLE handle = ::CreateFileW(
      path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW,
      FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    const DWORD error = ::GetLastError();
    return error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS
               ? OpenStatus::FileAlreadyExists
               : OpenStatus::IoError;
  }
  handle_ = handle;
#else
  descriptor_ = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0644);
  if (descriptor_ == -1) {
    return errno == EEXIST ? OpenStatus::FileAlreadyExists
                           : OpenStatus::IoError;
  }
#endif

  config_ = config;
  FileHeader header{};
  header.payload_size = config.payload_size;
  header.alignment = config.alignment;
  header.records_offset = records_offset(config);
  header.payload_schema_version = config.payload_schema_version;
  header.header_crc32 = file_header_crc32(header);
  const auto header_bytes = serialize_file_header(header);

  if (!write_bytes(header_bytes)) {
    (void)close();
    return OpenStatus::IoError;
  }

  const std::array<std::byte, default_alignment> zeros{};
  std::uint32_t remaining = file_header_padding_size(config);
  while (remaining != 0) {
    const std::uint32_t chunk = std::min(
        remaining, static_cast<std::uint32_t>(zeros.size()));
    if (!write_bytes({zeros.data(), chunk})) {
      (void)close();
      return OpenStatus::IoError;
    }
    remaining -= chunk;
  }

  if (!sync()
#if !defined(_WIN32)
      || !sync_parent_directory(path)
#endif
  ) {
    (void)close();
    return OpenStatus::IoError;
  }
  return OpenStatus::Ok;
}

bool PhysicalWalAdapter::append_record(
    std::uint64_t sequence,
    std::span<const std::byte> payload) noexcept {
  if (!is_open() || payload.size() != config_.payload_size ||
      inject_append_failure()) {
    return false;
  }

  RecordHeader header{};
  header.sequence = sequence;
  header.payload_crc32 = crc32_bytes(payload.data(), payload.size());
  header.header_crc32 = record_header_crc32(header);
  const auto header_bytes = serialize_record_header(header);

  if (!write_bytes(header_bytes) || !write_bytes(payload)) {
    return false;
  }

  const std::array<std::byte, default_alignment> zeros{};
  std::uint32_t remaining = padding_size(config_);
  while (remaining != 0) {
    const std::uint32_t chunk = std::min(
        remaining, static_cast<std::uint32_t>(zeros.size()));
    if (!write_bytes({zeros.data(), chunk})) {
      return false;
    }
    remaining -= chunk;
  }
  return true;
}

bool PhysicalWalAdapter::sync() noexcept {
  if (!is_open() || inject_sync_failure()) {
    return false;
  }

#if defined(_WIN32)
  return ::FlushFileBuffers(static_cast<HANDLE>(handle_)) != FALSE;
#elif defined(__APPLE__)
  return ::fsync(descriptor_) == 0;
#else
  return ::fdatasync(descriptor_) == 0;
#endif
}

bool PhysicalWalAdapter::close() noexcept {
  if (!is_open()) {
    return true;
  }

#if defined(_WIN32)
  const HANDLE handle = static_cast<HANDLE>(handle_);
  if (::CloseHandle(handle) == FALSE) {
    return false;
  }
  handle_ = nullptr;
  return true;
#else
  const int descriptor = descriptor_;
  descriptor_ = -1;
  return ::close(descriptor) == 0;
#endif
}

bool PhysicalWalAdapter::is_open() const noexcept {
#if defined(_WIN32)
  return handle_ != nullptr;
#else
  return descriptor_ != -1;
#endif
}

bool PhysicalWalAdapter::write_bytes(
    std::span<const std::byte> bytes) noexcept {
#if defined(_WIN32)
  while (!bytes.empty()) {
    const DWORD requested = static_cast<DWORD>(std::min<std::size_t>(
        bytes.size(), std::numeric_limits<DWORD>::max()));
    DWORD written{};
    if (::WriteFile(static_cast<HANDLE>(handle_), bytes.data(), requested,
                    &written, nullptr) == FALSE ||
        written == 0) {
      return false;
    }
    bytes = bytes.subspan(written);
  }
#else
  while (!bytes.empty()) {
    const std::size_t requested = std::min<std::size_t>(
        bytes.size(), static_cast<std::size_t>(
                          std::numeric_limits<ssize_t>::max()));
    const ssize_t written = ::write(descriptor_, bytes.data(), requested);
    if (written == -1) {
      if (errno == EINTR) {
        continue;
      }
      return false;
    }
    if (written == 0) {
      return false;
    }
    bytes = bytes.subspan(static_cast<std::size_t>(written));
  }
#endif
  return true;
}

void set_physical_wal_file_test_control(
    PhysicalWalFileTestControl* control) noexcept {
  test_control = control;
}

} // namespace detail
} // namespace fexma::wal
