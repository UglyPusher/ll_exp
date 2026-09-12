#pragma once

/**
 * @file record_tape_types.hpp
 * @brief Public value types for the in-memory RecordTape.
 */

#include <cstddef>
#include <cstdint>
#include <span>

namespace fexma::wal {

// RecordTape storage alignment. Physical WAL alignment is independent and is
// declared with the physical WAL configuration types.
inline constexpr std::uint32_t default_alignment = 64;

using Position = std::uint64_t; // Absolute zero-based RecordTape position.

enum class ViewStatus : std::uint8_t {
  Ok,
  Closed,
  Reclaimed,
  Unpublished
};

struct RecordView {
  Position position{};
  std::span<const std::byte> payload{};
};

struct AccessResult {
  ViewStatus status{ViewStatus::Closed};
  RecordView record{};

  [[nodiscard]] bool ok() const noexcept { return status == ViewStatus::Ok; }
};

enum class PublishStatus : std::uint8_t {
  Ok,
  Full,
  InvalidPayloadSize,
  PositionExhausted,
  Closed
};

struct PublishResult {
  PublishStatus status{PublishStatus::Closed};
  Position position{};

  [[nodiscard]] bool ok() const noexcept {
    return status == PublishStatus::Ok;
  }
};

} // namespace fexma::wal
