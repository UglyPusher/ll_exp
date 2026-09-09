#pragma once

/**
 * @file record.hpp
 * @brief Fixed application-record codec for Demo 006.
 */

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

namespace detail {

inline void put_u16(ApplicationPayload& payload, std::size_t offset,
                    std::uint16_t value) noexcept {
  for (std::size_t i = 0; i < sizeof(value); ++i) {
    payload[offset + i] =
        static_cast<std::byte>((value >> (i * 8u)) & 0xffu);
  }
}

inline void put_u32(ApplicationPayload& payload, std::size_t offset,
                    std::uint32_t value) noexcept {
  for (std::size_t i = 0; i < sizeof(value); ++i) {
    payload[offset + i] =
        static_cast<std::byte>((value >> (i * 8u)) & 0xffu);
  }
}

inline void put_u64(ApplicationPayload& payload, std::size_t offset,
                    std::uint64_t value) noexcept {
  for (std::size_t i = 0; i < sizeof(value); ++i) {
    payload[offset + i] =
        static_cast<std::byte>((value >> (i * 8u)) & 0xffu);
  }
}

[[nodiscard]] inline std::uint16_t get_u16(std::span<const std::byte> payload,
                                           std::size_t offset) noexcept {
  std::uint16_t value{};
  for (std::size_t i = 0; i < sizeof(value); ++i) {
    value |= static_cast<std::uint16_t>(
                 std::to_integer<std::uint8_t>(payload[offset + i]))
             << (i * 8u);
  }
  return value;
}

[[nodiscard]] inline std::uint32_t get_u32(std::span<const std::byte> payload,
                                           std::size_t offset) noexcept {
  std::uint32_t value{};
  for (std::size_t i = 0; i < sizeof(value); ++i) {
    value |= static_cast<std::uint32_t>(
                 std::to_integer<std::uint8_t>(payload[offset + i]))
             << (i * 8u);
  }
  return value;
}

[[nodiscard]] inline std::uint64_t get_u64(std::span<const std::byte> payload,
                                           std::size_t offset) noexcept {
  std::uint64_t value{};
  for (std::size_t i = 0; i < sizeof(value); ++i) {
    value |= static_cast<std::uint64_t>(
                 std::to_integer<std::uint8_t>(payload[offset + i]))
             << (i * 8u);
  }
  return value;
}

} // namespace detail

[[nodiscard]] inline ApplicationPayload
encode_data(const ApplicationData& data) noexcept {
  ApplicationPayload payload{};
  detail::put_u32(payload, 0u, kApplicationMagic);
  detail::put_u16(payload, 4u, kApplicationVersion);
  detail::put_u16(payload, 6u,
                  static_cast<std::uint16_t>(RecordKind::Data));
  std::copy(data.begin(), data.end(), payload.begin() + 16u);
  return payload;
}

[[nodiscard]] inline ApplicationPayload
encode_save_snapshot(std::uint64_t generation_id) noexcept {
  ApplicationPayload payload{};
  detail::put_u32(payload, 0u, kApplicationMagic);
  detail::put_u16(payload, 4u, kApplicationVersion);
  detail::put_u16(payload, 6u,
                  static_cast<std::uint16_t>(RecordKind::SaveSnapshot));
  detail::put_u64(payload, 8u, generation_id);
  return payload;
}

[[nodiscard]] inline DecodeResult
decode_record(std::span<const std::byte> payload) noexcept {
  if (payload.size() != kApplicationPayloadSize) {
    return {DecodeStatus::InvalidSize, {}};
  }
  if (detail::get_u32(payload, 0u) != kApplicationMagic) {
    return {DecodeStatus::InvalidMagic, {}};
  }
  if (detail::get_u16(payload, 4u) != kApplicationVersion) {
    return {DecodeStatus::UnsupportedVersion, {}};
  }

  const auto raw_kind = detail::get_u16(payload, 6u);
  if (raw_kind != static_cast<std::uint16_t>(RecordKind::Data) &&
      raw_kind != static_cast<std::uint16_t>(RecordKind::SaveSnapshot)) {
    return {DecodeStatus::InvalidKind, {}};
  }

  ApplicationRecordView record{};
  record.kind = static_cast<RecordKind>(raw_kind);
  record.generation_id = detail::get_u64(payload, 8u);
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
