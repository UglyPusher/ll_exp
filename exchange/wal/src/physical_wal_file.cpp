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

template <class T>
[[nodiscard]] std::span<const std::byte> as_bytes(const T& value) noexcept {
  return {reinterpret_cast<const std::byte*>(&value), sizeof(T)};
}

[[nodiscard]] std::uint32_t padding_size(const WalConfig& config) noexcept {
  const std::uint64_t raw_size = sizeof(RecordHeader) + config.payload_size;
  return static_cast<std::uint32_t>(aligned_record_size(config) - raw_size);
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

PhysicalWalFile::~PhysicalWalFile() { (void)close(); }

bool PhysicalWalFile::create(const std::filesystem::path& path,
                             const WalConfig& config) noexcept {
  if (is_open()) {
    return false;
  }

#if defined(_WIN32)
  const HANDLE handle = ::CreateFileW(
      path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
      FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    return false;
  }
  handle_ = handle;
#else
  descriptor_ = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (descriptor_ == -1) {
    return false;
  }
#endif

  config_ = config;
  FileHeader header{};
  header.payload_size = config.payload_size;
  header.alignment = config.alignment;
  header.header_crc32 = file_header_crc32(header);

  if (!write_bytes(as_bytes(header)) || !sync()
#if !defined(_WIN32)
      || !sync_parent_directory(path)
#endif
  ) {
    (void)close();
    return false;
  }
  return true;
}

bool PhysicalWalFile::append_record(
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

  if (!write_bytes(as_bytes(header)) || !write_bytes(payload)) {
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

bool PhysicalWalFile::sync() noexcept {
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

bool PhysicalWalFile::close() noexcept {
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

bool PhysicalWalFile::is_open() const noexcept {
#if defined(_WIN32)
  return handle_ != nullptr;
#else
  return descriptor_ != -1;
#endif
}

bool PhysicalWalFile::write_bytes(
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
