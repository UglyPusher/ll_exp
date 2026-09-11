#pragma once

/**
 * @file record.hpp
 * @brief Fixed application-record codec for Demo 006.
 */

#include <fexma/binary/little_endian.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace fexma::snapshot_demo {

inline constexpr std::uint32_t kApplicationMagic = 0x36303044u; // "D006"
inline constexpr std::uint16_t kApplicationVersion = 1u;
inline constexpr std::size_t kApplicationPayloadSize = 64u;
inline constexpr std::size_t kApplicationDataSize = 48u;

using ApplicationPayload = std::array<std::byte, kApplicationPayloadSize>;
using ApplicationData = std::array<std::byte, kApplicationDataSize>;

enum class RecordKind : std::uint16_t {
  Data = 1u,
  SaveSnapshot = 2u,
};

struct ApplicationRecordView {
  RecordKind kind{};
  std::uint64_t generation_id{};
  std::span<const std::byte> data{};
};

enum class DecodeStatus {
  Ok,
  InvalidSize,
  InvalidMagic,
  UnsupportedVersion,
  InvalidKind,
  InvalidGeneration,
  NonZeroSnapshotData,
};

struct DecodeResult {
  DecodeStatus status{DecodeStatus::InvalidSize};
  ApplicationRecordView record{};

  [[nodiscard]] explicit operator bool() const noexcept {
    return status == DecodeStatus::Ok;
  }
};

[[nodiscard]] inline ApplicationPayload
encode_data(const ApplicationData& data) noexcept {
  ApplicationPayload payload{};
  binary::store_le(payload, 0u, kApplicationMagic);
  binary::store_le(payload, 4u, kApplicationVersion);
  binary::store_le(payload, 6u,
                   static_cast<std::uint16_t>(RecordKind::Data));
  std::copy(data.begin(), data.end(), payload.begin() + 16u);
  return payload;
}

[[nodiscard]] inline ApplicationPayload
encode_save_snapshot(std::uint64_t generation_id) noexcept {
  ApplicationPayload payload{};
  binary::store_le(payload, 0u, kApplicationMagic);
  binary::store_le(payload, 4u, kApplicationVersion);
  binary::store_le(payload, 6u,
                   static_cast<std::uint16_t>(RecordKind::SaveSnapshot));
  binary::store_le(payload, 8u, generation_id);
  return payload;
}

[[nodiscard]] inline DecodeResult
decode_record(std::span<const std::byte> payload) noexcept {
  if (payload.size() != kApplicationPayloadSize) {
    return {DecodeStatus::InvalidSize, {}};
  }
  if (binary::load_le<std::uint32_t>(payload, 0u) != kApplicationMagic) {
    return {DecodeStatus::InvalidMagic, {}};
  }
  if (binary::load_le<std::uint16_t>(payload, 4u) != kApplicationVersion) {
    return {DecodeStatus::UnsupportedVersion, {}};
  }

  const auto raw_kind = binary::load_le<std::uint16_t>(payload, 6u);
  if (raw_kind != static_cast<std::uint16_t>(RecordKind::Data) &&
      raw_kind != static_cast<std::uint16_t>(RecordKind::SaveSnapshot)) {
    return {DecodeStatus::InvalidKind, {}};
  }

  ApplicationRecordView record{};
  record.kind = static_cast<RecordKind>(raw_kind);
  record.generation_id = binary::load_le<std::uint64_t>(payload, 8u);
  record.data = payload.subspan(16u, kApplicationDataSize);

  if (record.kind == RecordKind::Data && record.generation_id != 0u) {
    return {DecodeStatus::InvalidGeneration, {}};
  }
  if (record.kind == RecordKind::SaveSnapshot) {
    for (const std::byte value : record.data) {
      if (value != std::byte{}) {
        return {DecodeStatus::NonZeroSnapshotData, {}};
      }
    }
  }
  return {DecodeStatus::Ok, record};
}

} // namespace fexma::snapshot_demo
