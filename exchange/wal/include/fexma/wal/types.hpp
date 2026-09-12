#pragma once

/**
 * @file types.hpp
 * @brief Public types for the physical WAL format and lifecycle.
 */

#include <cstdint>

namespace fexma::wal {

inline constexpr std::uint32_t file_magic = 0x57414c46u;   // FLAW
inline constexpr std::uint32_t record_magic = 0x57414c52u; // RLAW
inline constexpr std::uint16_t format_version = 3;
inline constexpr std::uint32_t wal_default_alignment = 64;

using StreamId = std::uint64_t;
using EpochId = std::uint64_t;
using ManifestId = std::uint64_t;
enum class StreamKind : std::uint16_t {
  Generic = 0,
  Command = 1,
  Event = 2
};

struct WalConfig {
  std::uint32_t payload_size{};
  std::uint32_t capacity{};
  std::uint32_t alignment{wal_default_alignment};
  std::uint32_t payload_schema_version{};
  StreamKind stream_kind{StreamKind::Generic};
  StreamId stream_id{};
  EpochId epoch_id{};
  std::uint64_t first_sequence{1};
  ManifestId manifest_id{};
};

enum class OpenStatus : std::uint8_t {
  Ok,
  InvalidConfig,
  AllocationFailed,
  IoError,
  FileAlreadyExists,
  AlreadyOpen
};

struct OpenResult {
  OpenStatus status{OpenStatus::IoError};

  [[nodiscard]] bool ok() const noexcept { return status == OpenStatus::Ok; }
};

} // namespace fexma::wal
