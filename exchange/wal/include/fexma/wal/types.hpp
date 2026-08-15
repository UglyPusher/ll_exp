#pragma once

/**
 * @file types.hpp
 * @brief Public types for the fixed-payload durable queue.
 */

#include <cstdint>

namespace fexma::wal {

inline constexpr std::uint32_t file_magic = 0x57414c46u;   // FLAW
inline constexpr std::uint32_t record_magic = 0x57414c52u; // RLAW
inline constexpr std::uint16_t format_version = 1;
inline constexpr std::uint32_t default_alignment = 64;

struct WalConfig {
  std::uint32_t payload_size{};
  std::uint32_t capacity{};
  std::uint32_t alignment{default_alignment};
};

enum class OpenStatus : std::uint8_t {
  Ok,
  InvalidConfig,
  AllocationFailed,
  IoError,
  AlreadyOpen
};

struct OpenResult {
  OpenStatus status{OpenStatus::IoError};

  [[nodiscard]] bool ok() const noexcept { return status == OpenStatus::Ok; }
};

enum class PushStatus : std::uint8_t {
  Ok,
  Full,
  InvalidPayloadSize,
  IoError,
  Closed
};

struct PushResult {
  PushStatus status{PushStatus::Closed};
  std::uint64_t sequence{};

  [[nodiscard]] bool ok() const noexcept { return status == PushStatus::Ok; }
};

enum class DurabilityStatus : std::uint8_t {
  Ok,
  IoError,
  Closed
};

struct DurabilityResult {
  DurabilityStatus status{DurabilityStatus::Closed};
  std::uint64_t durable_cursor{};
  std::uint32_t records{};

  [[nodiscard]] bool ok() const noexcept {
    return status == DurabilityStatus::Ok;
  }
};

enum class PopStatus : std::uint8_t {
  Ok,
  Empty,
  InvalidPayloadSize,
  Closed
};

struct PopResult {
  PopStatus status{PopStatus::Closed};
  std::uint64_t sequence{};

  [[nodiscard]] bool ok() const noexcept { return status == PopStatus::Ok; }
};

enum class CloseStatus : std::uint8_t {
  Ok,
  PendingDurability,
  IoError,
  AlreadyClosed
};

struct CloseResult {
  CloseStatus status{CloseStatus::AlreadyClosed};

  [[nodiscard]] bool ok() const noexcept { return status == CloseStatus::Ok; }
};

} // namespace fexma::wal
