#pragma once

/**
 * @file types.hpp
 * @brief Public types for the fixed-payload WAL frontier ring.
 */

#include <cstdint>

namespace fexma::wal {

inline constexpr std::uint32_t file_magic = 0x57414c46u;   // FLAW
inline constexpr std::uint32_t record_magic = 0x57414c52u; // RLAW
inline constexpr std::uint16_t format_version = 2;
inline constexpr std::uint32_t default_alignment = 64;

struct WalConfig {
  std::uint32_t payload_size{};
  std::uint32_t capacity{};
  std::uint32_t alignment{default_alignment};
  std::uint32_t payload_schema_version{};
};

struct WalSnapshot {
  std::uint64_t tail{};
  std::uint64_t durable{};
  std::uint64_t head{};
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

enum class DurabilityStatus : std::uint8_t {
  Ok,
  IoError,
  Closed
};

struct DurabilityResult {
  DurabilityStatus status{DurabilityStatus::Closed};
  std::uint64_t durable_frontier{};
  std::uint32_t records{};

  [[nodiscard]] bool ok() const noexcept {
    return status == DurabilityStatus::Ok;
  }
};

enum class ConsumeStatus : std::uint8_t {
  Ok,
  Empty,
  InvalidPayloadSize,
  Closed
};

struct ConsumeResult {
  ConsumeStatus status{ConsumeStatus::Closed};
  std::uint64_t sequence{};

  [[nodiscard]] bool ok() const noexcept {
    return status == ConsumeStatus::Ok;
  }
};

enum class CloseStatus : std::uint8_t {
  Ok,
  PendingConsumption,
  PendingDurability,
  IoError,
  AlreadyClosed
};

struct CloseResult {
  CloseStatus status{CloseStatus::AlreadyClosed};

  [[nodiscard]] bool ok() const noexcept { return status == CloseStatus::Ok; }
};

} // namespace fexma::wal
