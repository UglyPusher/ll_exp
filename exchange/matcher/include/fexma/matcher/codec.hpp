/**
 * @file codec.hpp
 * @brief Canonical Command and Event WAL payload codecs.
 */
#pragma once

#include <fexma/matcher/types.hpp>

#include <cstddef>
#include <cstdint>
#include <span>

namespace fexma::matcher {

inline constexpr std::size_t command_wal_payload_size_v1 = 48;
inline constexpr std::size_t event_wal_payload_size_v1 = 64;
inline constexpr std::size_t command_wal_payload_size_v2 = 48;
inline constexpr std::size_t event_wal_payload_size_v2 = 64;

enum class PayloadCodecStatus : std::uint8_t {
  Ok,
  InvalidSize,
  InvalidType,
  InvalidField,
  NonCanonicalBytes
};

[[nodiscard]] PayloadCodecStatus encode_command_wal_payload_v1(
    const CommandWalPayload& payload, std::span<std::byte> bytes) noexcept;

[[nodiscard]] PayloadCodecStatus decode_command_wal_payload_v1(
    std::span<const std::byte> bytes, CommandWalPayload& payload) noexcept;

[[nodiscard]] PayloadCodecStatus encode_event_wal_payload_v1(
    const EventWalPayload& payload, std::span<std::byte> bytes) noexcept;

[[nodiscard]] PayloadCodecStatus decode_event_wal_payload_v1(
    std::span<const std::byte> bytes, EventWalPayload& payload) noexcept;

[[nodiscard]] PayloadCodecStatus encode_command_wal_payload_v2(
    const CommandWalPayload& payload, std::span<std::byte> bytes) noexcept;

[[nodiscard]] PayloadCodecStatus decode_command_wal_payload_v2(
    std::span<const std::byte> bytes, CommandWalPayload& payload) noexcept;

[[nodiscard]] PayloadCodecStatus encode_event_wal_payload_v2(
    const EventWalPayload& payload, std::span<std::byte> bytes) noexcept;

[[nodiscard]] PayloadCodecStatus decode_event_wal_payload_v2(
    std::span<const std::byte> bytes, EventWalPayload& payload) noexcept;

} // namespace fexma::matcher
