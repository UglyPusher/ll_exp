#pragma once

/**
 * @file types.hpp
 * @brief Public types for the fixed-payload WAL frontier ring.
 */

#include <cstddef>
#include <cstdint>
#include <span>

namespace fexma::wal {

inline constexpr std::uint32_t file_magic = 0x57414c46u;   // FLAW
inline constexpr std::uint32_t record_magic = 0x57414c52u; // RLAW
inline constexpr std::uint16_t format_version = 3;
inline constexpr std::uint32_t default_alignment = 64;

using StreamId = std::uint64_t;
using EpochId = std::uint64_t;
using ManifestId = std::uint64_t;
using Position = std::uint64_t; // Absolute zero-based runtime WAL position.

enum class ViewStatus : std::uint8_t {
  Ok,
  Closed,
  Reclaimed,
  Unpublished
};

struct RecordView {
  Position position{};
  std::uint64_t sequence{};
  std::span<const std::byte> payload{};
};

struct AccessResult {
  ViewStatus status{ViewStatus::Closed};
  RecordView record{};

  [[nodiscard]] bool ok() const noexcept { return status == ViewStatus::Ok; }
};

enum class StreamKind : std::uint16_t {
  Generic = 0,
  Command = 1,
  Event = 2
};

struct WalConfig {
  std::uint32_t payload_size{};
  std::uint32_t capacity{};
  std::uint32_t alignment{default_alignment};
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

enum class PublishStatus : std::uint8_t {
  Ok,
  Full,
  InvalidPayloadSize,
  SequenceExhausted,
  IoError,
  Closed
};

struct PublishResult {
  PublishStatus status{PublishStatus::Closed};
  std::uint64_t sequence{};

  [[nodiscard]] bool ok() const noexcept {
    return status == PublishStatus::Ok;
  }
};

} // namespace fexma::wal
