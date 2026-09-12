#include "physical_wal_file.hpp"

#include <fexma/binary/little_endian.hpp>

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
  binary::store_le(out, 0, header.magic);
  binary::store_le(out, 4, header.version);
  binary::store_le(out, 6, header.header_size);
  binary::store_le(out, 8, static_cast<std::uint16_t>(header.stream_kind));
  binary::store_le(out, 10, header.flags);
  binary::store_le(out, 12, header.payload_size);
  binary::store_le(out, 16, header.payload_schema_version);
  binary::store_le(out, 20, header.alignment);
  binary::store_le(out, 24, header.records_offset);
  binary::store_le(out, 28, header.stream_id);
  binary::store_le(out, 36, header.epoch_id);
  binary::store_le(out, 44, header.first_sequence);
  binary::store_le(out, 52, header.manifest_id);
  binary::store_le(out, 60, header.header_crc32);
  return out;
}

std::array<std::byte, physical_record_header_size>
serialize_record_header(RecordHeader header) noexcept {
  std::array<std::byte, physical_record_header_size> out{};
  binary::store_le(out, 0, header.magic);
  binary::store_le(out, 4, header.version);
  binary::store_le(out, 6, header.header_size);
  binary::store_le(out, 8, header.sequence);
  binary::store_le(out, 16, header.payload_crc32);
  binary::store_le(out, 20, header.header_crc32);
  return out;
}

bool deserialize_file_header(std::span<const std::byte> bytes,
                             FileHeader& header) noexcept {
  if (bytes.size() != physical_file_header_size) {
    return false;
  }
  header.magic = binary::load_le<std::uint32_t>(bytes, 0);
  header.version = binary::load_le<std::uint16_t>(bytes, 4);
  header.header_size = binary::load_le<std::uint16_t>(bytes, 6);
  header.stream_kind = static_cast<StreamKind>(binary::load_le<std::uint16_t>(bytes, 8));
  header.flags = binary::load_le<std::uint16_t>(bytes, 10);
  header.payload_size = binary::load_le<std::uint32_t>(bytes, 12);
  header.payload_schema_version = binary::load_le<std::uint32_t>(bytes, 16);
  header.alignment = binary::load_le<std::uint32_t>(bytes, 20);
  header.records_offset = binary::load_le<std::uint32_t>(bytes, 24);
  header.stream_id = binary::load_le<std::uint64_t>(bytes, 28);
  header.epoch_id = binary::load_le<std::uint64_t>(bytes, 36);
  header.first_sequence = binary::load_le<std::uint64_t>(bytes, 44);
  header.manifest_id = binary::load_le<std::uint64_t>(bytes, 52);
  header.header_crc32 = binary::load_le<std::uint32_t>(bytes, 60);
  return true;
}

bool deserialize_record_header(std::span<const std::byte> bytes,
                               RecordHeader& header) noexcept {
  if (bytes.size() != physical_record_header_size) {
    return false;
  }
  header.magic = binary::load_le<std::uint32_t>(bytes, 0);
  header.version = binary::load_le<std::uint16_t>(bytes, 4);
  header.header_size = binary::load_le<std::uint16_t>(bytes, 6);
  header.sequence = binary::load_le<std::uint64_t>(bytes, 8);
  header.payload_crc32 = binary::load_le<std::uint32_t>(bytes, 16);
  header.header_crc32 = binary::load_le<std::uint32_t>(bytes, 20);
  return true;
}

namespace detail {
namespace {

PhysicalWalFileTestControl* test_control{};
PhysicalWalRecoveryTestControl* recovery_test_control{};

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

[[nodiscard]] bool inject_truncate_failure() noexcept {
  if (recovery_test_control == nullptr) {
    return false;
  }
  const std::uint64_t call = recovery_test_control->truncate_calls++;
  return call == recovery_test_control->fail_truncate_call;
}

[[nodiscard]] bool inject_recovery_sync_failure() noexcept {
  if (recovery_test_control == nullptr) {
    return false;
  }
  const std::uint64_t call = recovery_test_control->sync_calls++;
  return call == recovery_test_control->fail_sync_call;
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
  header.stream_kind = config.stream_kind;
  header.payload_size = config.payload_size;
  header.payload_schema_version = config.payload_schema_version;
  header.alignment = config.alignment;
  header.records_offset = records_offset(config);
  header.stream_id = config.stream_id;
  header.epoch_id = config.epoch_id;
  header.first_sequence = config.first_sequence;
  header.manifest_id = config.manifest_id;
  header.header_crc32 = file_header_crc32(header);
  const auto header_bytes = serialize_file_header(header);

  if (!write_bytes(header_bytes)) {
    (void)close();
    return OpenStatus::IoError;
  }

  const std::array<std::byte, wal_default_alignment> zeros{};
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

  const std::array<std::byte, wal_default_alignment> zeros{};
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

PhysicalWalReaderAdapter::~PhysicalWalReaderAdapter() { (void)close(); }

bool PhysicalWalReaderAdapter::open(
    const std::filesystem::path& path) noexcept {
  if (is_open()) {
    return false;
  }
#if defined(_WIN32)
  const HANDLE handle = ::CreateFileW(
      path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    return false;
  }
  handle_ = handle;
#else
  descriptor_ = ::open(path.c_str(), O_RDONLY);
  if (descriptor_ == -1) {
    return false;
  }
#endif
  return true;
}

PhysicalReadStatus
PhysicalWalReaderAdapter::read(std::span<std::byte> bytes) noexcept {
  if (!is_open()) {
    return PhysicalReadStatus::IoError;
  }
  std::size_t completed{};
#if defined(_WIN32)
  while (!bytes.empty()) {
    const DWORD requested = static_cast<DWORD>(std::min<std::size_t>(
        bytes.size(), std::numeric_limits<DWORD>::max()));
    DWORD read_bytes{};
    if (::ReadFile(static_cast<HANDLE>(handle_), bytes.data(), requested,
                   &read_bytes, nullptr) == FALSE) {
      return PhysicalReadStatus::IoError;
    }
    if (read_bytes == 0) {
      return completed == 0 ? PhysicalReadStatus::EndOfFile
                            : PhysicalReadStatus::Incomplete;
    }
    completed += read_bytes;
    bytes = bytes.subspan(read_bytes);
  }
#else
  while (!bytes.empty()) {
    const std::size_t requested = std::min<std::size_t>(
        bytes.size(), static_cast<std::size_t>(
                          std::numeric_limits<ssize_t>::max()));
    const ssize_t read_bytes = ::read(descriptor_, bytes.data(), requested);
    if (read_bytes == -1) {
      if (errno == EINTR) {
        continue;
      }
      return PhysicalReadStatus::IoError;
    }
    if (read_bytes == 0) {
      return completed == 0 ? PhysicalReadStatus::EndOfFile
                            : PhysicalReadStatus::Incomplete;
    }
    completed += static_cast<std::size_t>(read_bytes);
    bytes = bytes.subspan(static_cast<std::size_t>(read_bytes));
  }
#endif
  return PhysicalReadStatus::Complete;
}

bool PhysicalWalReaderAdapter::close() noexcept {
  if (!is_open()) {
    return true;
  }
#if defined(_WIN32)
  const HANDLE handle = static_cast<HANDLE>(handle_);
  handle_ = nullptr;
  return ::CloseHandle(handle) != FALSE;
#else
  const int descriptor = descriptor_;
  descriptor_ = -1;
  return ::close(descriptor) == 0;
#endif
}

bool PhysicalWalReaderAdapter::is_open() const noexcept {
#if defined(_WIN32)
  return handle_ != nullptr;
#else
  return descriptor_ != -1;
#endif
}

bool PhysicalWalRecoveryAdapter::truncate_and_sync(
    const std::filesystem::path& path, std::uint64_t size) noexcept {
  if (inject_truncate_failure()) {
    return false;
  }

#if defined(_WIN32)
  if (size > static_cast<std::uint64_t>(
                 std::numeric_limits<LONGLONG>::max())) {
    return false;
  }
  const HANDLE handle = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                                      nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    return false;
  }
  LARGE_INTEGER offset{};
  offset.QuadPart = static_cast<LONGLONG>(size);
  const bool truncated = ::SetFilePointerEx(handle, offset, nullptr,
                                             FILE_BEGIN) != FALSE &&
                         ::SetEndOfFile(handle) != FALSE;
  const bool synced = truncated && !inject_recovery_sync_failure() &&
                      ::FlushFileBuffers(handle) != FALSE;
  const bool closed = ::CloseHandle(handle) != FALSE;
  return truncated && synced && closed;
#else
  if (size > static_cast<std::uint64_t>(
                 std::numeric_limits<off_t>::max())) {
    return false;
  }
  const int descriptor = ::open(path.c_str(), O_WRONLY);
  if (descriptor == -1) {
    return false;
  }
  const bool truncated =
      ::ftruncate(descriptor, static_cast<off_t>(size)) == 0;
#if defined(__APPLE__)
  const bool synced = truncated && !inject_recovery_sync_failure() &&
                      ::fsync(descriptor) == 0;
#else
  const bool synced = truncated && !inject_recovery_sync_failure() &&
                      ::fdatasync(descriptor) == 0;
#endif
  const bool closed = ::close(descriptor) == 0;
  return truncated && synced && closed;
#endif
}

void set_physical_wal_file_test_control(
    PhysicalWalFileTestControl* control) noexcept {
  test_control = control;
}

void set_physical_wal_recovery_test_control(
    PhysicalWalRecoveryTestControl* control) noexcept {
  recovery_test_control = control;
}

} // namespace detail
} // namespace fexma::wal
