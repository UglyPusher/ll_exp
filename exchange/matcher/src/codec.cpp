/**
 * @file codec.cpp
 * @brief Canonical little-endian Command and Event WAL payload codec.
 */
#include <fexma/matcher/codec.hpp>

#include <algorithm>
#include <array>

namespace fexma::matcher {
namespace {

void write_u32(std::span<std::byte> bytes, std::size_t offset,
               std::uint32_t value) noexcept {
  for (std::size_t index = 0; index < 4; ++index) {
    bytes[offset + index] =
        static_cast<std::byte>((value >> (index * 8)) & 0xffu);
  }
}

void write_u64(std::span<std::byte> bytes, std::size_t offset,
               std::uint64_t value) noexcept {
  for (std::size_t index = 0; index < 8; ++index) {
    bytes[offset + index] =
        static_cast<std::byte>((value >> (index * 8)) & 0xffu);
  }
}

[[nodiscard]] std::uint32_t read_u32(std::span<const std::byte> bytes,
                                     std::size_t offset) noexcept {
  std::uint32_t value{};
  for (std::size_t index = 0; index < 4; ++index) {
    value |= static_cast<std::uint32_t>(bytes[offset + index]) << (index * 8);
  }
  return value;
}

[[nodiscard]] std::uint64_t read_u64(std::span<const std::byte> bytes,
                                     std::size_t offset) noexcept {
  std::uint64_t value{};
  for (std::size_t index = 0; index < 8; ++index) {
    value |= static_cast<std::uint64_t>(bytes[offset + index]) << (index * 8);
  }
  return value;
}

[[nodiscard]] bool valid_side(std::uint8_t value) noexcept {
  return value <= static_cast<std::uint8_t>(Side::Ask);
}

[[nodiscard]] bool valid_reject_reason(std::uint8_t value) noexcept {
  return value <= static_cast<std::uint8_t>(RejectReason::UnknownCommand);
}

[[nodiscard]] bool valid_fatal_reason(std::uint8_t value) noexcept {
  return value <= static_cast<std::uint8_t>(FatalReason::SnapshotLoadInvalid);
}

template <std::size_t Size>
[[nodiscard]] bool equals(std::span<const std::byte> bytes,
                          const std::array<std::byte, Size>& expected) noexcept {
  return std::equal(bytes.begin(), bytes.end(), expected.begin());
}

} // namespace

PayloadCodecStatus encode_command_wal_payload_v1(
    const CommandWalPayload& payload, std::span<std::byte> bytes) noexcept {
  if (bytes.size() != command_wal_payload_size_v1) {
    return PayloadCodecStatus::InvalidSize;
  }

  std::fill(bytes.begin(), bytes.end(), std::byte{});
  write_u64(bytes, 0, payload.client_id);
  bytes[8] = static_cast<std::byte>(payload.message.type);

  switch (payload.message.type) {
  case CommandType::NewLimit: {
    const NewLimitOrder& command = payload.message.new_limit;
    const std::uint8_t side = static_cast<std::uint8_t>(command.side);
    if (!valid_side(side)) {
      return PayloadCodecStatus::InvalidField;
    }
    write_u64(bytes, 16, command.id);
    write_u64(bytes, 24, command.owner_id);
    bytes[32] = static_cast<std::byte>(side);
    write_u32(bytes, 36, command.price);
    write_u32(bytes, 40, command.quantity);
    return PayloadCodecStatus::Ok;
  }
  case CommandType::SaveSnapshot:
    write_u64(bytes, 16, payload.message.save_snapshot.snapshot_id);
    write_u64(bytes, 24, payload.message.save_snapshot.snapshot_epoch_id);
    return PayloadCodecStatus::Ok;
  case CommandType::LoadSnapshot:
    write_u64(bytes, 16, payload.message.load_snapshot.snapshot_id);
    write_u64(bytes, 24, payload.message.load_snapshot.snapshot_epoch_id);
    return PayloadCodecStatus::Ok;
  case CommandType::Shutdown:
    return PayloadCodecStatus::Ok;
  case CommandType::None:
    return PayloadCodecStatus::InvalidType;
  }
  return PayloadCodecStatus::InvalidType;
}

PayloadCodecStatus decode_command_wal_payload_v1(
    std::span<const std::byte> bytes, CommandWalPayload& payload) noexcept {
  if (bytes.size() != command_wal_payload_size_v1) {
    return PayloadCodecStatus::InvalidSize;
  }

  CommandWalPayload decoded{};
  decoded.client_id = read_u64(bytes, 0);
  const auto type = static_cast<CommandType>(bytes[8]);

  switch (type) {
  case CommandType::NewLimit: {
    const std::uint8_t side = static_cast<std::uint8_t>(bytes[32]);
    if (!valid_side(side)) {
      return PayloadCodecStatus::InvalidField;
    }
    decoded.message = Command{NewLimitOrder{
        read_u64(bytes, 16), read_u64(bytes, 24), static_cast<Side>(side),
        read_u32(bytes, 36), read_u32(bytes, 40)}};
    break;
  }
  case CommandType::SaveSnapshot:
    decoded.message = Command{SaveSnapshotCommand{
        read_u64(bytes, 16), read_u64(bytes, 24)}};
    break;
  case CommandType::LoadSnapshot:
    decoded.message = Command{LoadSnapshotCommand{
        read_u64(bytes, 16), read_u64(bytes, 24)}};
    break;
  case CommandType::Shutdown:
    decoded.message = Command{ShutdownCommand{}};
    break;
  case CommandType::None:
    return PayloadCodecStatus::InvalidType;
  default:
    return PayloadCodecStatus::InvalidType;
  }

  std::array<std::byte, command_wal_payload_size_v1> canonical{};
  const PayloadCodecStatus encoded =
      encode_command_wal_payload_v1(decoded, canonical);
  if (encoded != PayloadCodecStatus::Ok) {
    return encoded;
  }
  if (!equals(bytes, canonical)) {
    return PayloadCodecStatus::NonCanonicalBytes;
  }

  payload = decoded;
  return PayloadCodecStatus::Ok;
}

PayloadCodecStatus encode_event_wal_payload_v1(
    const EventWalPayload& payload, std::span<std::byte> bytes) noexcept {
  if (bytes.size() != event_wal_payload_size_v1) {
    return PayloadCodecStatus::InvalidSize;
  }

  std::fill(bytes.begin(), bytes.end(), std::byte{});
  write_u64(bytes, 0, payload.client_id);
  write_u64(bytes, 8, payload.caused_by_command_sequence);
  write_u32(bytes, 16, payload.index_in_command);
  bytes[20] = payload.is_last_for_command ? std::byte{1} : std::byte{0};
  bytes[21] = static_cast<std::byte>(payload.message.type);

  switch (payload.message.type) {
  case EventType::OrderAccepted:
    write_u64(bytes, 24, payload.message.accepted.id);
    return PayloadCodecStatus::Ok;
  case EventType::OrderRejected: {
    const std::uint8_t reason =
        static_cast<std::uint8_t>(payload.message.rejected.reason);
    if (!valid_reject_reason(reason)) {
      return PayloadCodecStatus::InvalidField;
    }
    write_u64(bytes, 24, payload.message.rejected.id);
    bytes[32] = static_cast<std::byte>(reason);
    return PayloadCodecStatus::Ok;
  }
  case EventType::Trade:
    write_u64(bytes, 24, payload.message.trade.taker_order_id);
    write_u64(bytes, 32, payload.message.trade.maker_order_id);
    write_u64(bytes, 40, payload.message.trade.taker_owner_id);
    write_u64(bytes, 48, payload.message.trade.maker_owner_id);
    write_u32(bytes, 56, payload.message.trade.price);
    write_u32(bytes, 60, payload.message.trade.quantity);
    return PayloadCodecStatus::Ok;
  case EventType::OrderRested: {
    const std::uint8_t side =
        static_cast<std::uint8_t>(payload.message.rested.side);
    if (!valid_side(side)) {
      return PayloadCodecStatus::InvalidField;
    }
    write_u64(bytes, 24, payload.message.rested.id);
    write_u64(bytes, 32, payload.message.rested.owner_id);
    bytes[40] = static_cast<std::byte>(side);
    write_u32(bytes, 44, payload.message.rested.price);
    write_u32(bytes, 48, payload.message.rested.remaining);
    return PayloadCodecStatus::Ok;
  }
  case EventType::OrderDone:
    write_u64(bytes, 24, payload.message.done.id);
    return PayloadCodecStatus::Ok;
  case EventType::SaveSnapshot:
    write_u64(bytes, 24, payload.message.save_snapshot.snapshot_id);
    write_u64(bytes, 32, payload.message.save_snapshot.snapshot_epoch_id);
    return PayloadCodecStatus::Ok;
  case EventType::LoadSnapshot:
    write_u64(bytes, 24, payload.message.load_snapshot.snapshot_id);
    write_u64(bytes, 32, payload.message.load_snapshot.snapshot_epoch_id);
    return PayloadCodecStatus::Ok;
  case EventType::Shutdown:
    return PayloadCodecStatus::Ok;
  case EventType::MatcherFatal: {
    const std::uint8_t reason =
        static_cast<std::uint8_t>(payload.message.fatal.reason);
    if (!valid_fatal_reason(reason)) {
      return PayloadCodecStatus::InvalidField;
    }
    bytes[24] = static_cast<std::byte>(reason);
    write_u64(bytes, 32, payload.message.fatal.offending_order_id);
    write_u64(bytes, 40, payload.message.fatal.last_order_id);
    return PayloadCodecStatus::Ok;
  }
  case EventType::None:
    return PayloadCodecStatus::InvalidType;
  }
  return PayloadCodecStatus::InvalidType;
}

PayloadCodecStatus decode_event_wal_payload_v1(
    std::span<const std::byte> bytes, EventWalPayload& payload) noexcept {
  if (bytes.size() != event_wal_payload_size_v1) {
    return PayloadCodecStatus::InvalidSize;
  }

  const std::uint8_t final_flag = static_cast<std::uint8_t>(bytes[20]);
  if (final_flag > 1) {
    return PayloadCodecStatus::InvalidField;
  }

  EventWalPayload decoded{};
  decoded.client_id = read_u64(bytes, 0);
  decoded.caused_by_command_sequence = read_u64(bytes, 8);
  decoded.index_in_command = read_u32(bytes, 16);
  decoded.is_last_for_command = final_flag != 0;
  const auto type = static_cast<EventType>(bytes[21]);

  switch (type) {
  case EventType::OrderAccepted:
    decoded.message = Event{OrderAcceptedEvent{read_u64(bytes, 24)}};
    break;
  case EventType::OrderRejected: {
    const std::uint8_t reason = static_cast<std::uint8_t>(bytes[32]);
    if (!valid_reject_reason(reason)) {
      return PayloadCodecStatus::InvalidField;
    }
    decoded.message = Event{OrderRejectedEvent{
        read_u64(bytes, 24), static_cast<RejectReason>(reason)}};
    break;
  }
  case EventType::Trade:
    decoded.message = Event{TradeEvent{
        read_u64(bytes, 24), read_u64(bytes, 32), read_u64(bytes, 40),
        read_u64(bytes, 48), read_u32(bytes, 56), read_u32(bytes, 60)}};
    break;
  case EventType::OrderRested: {
    const std::uint8_t side = static_cast<std::uint8_t>(bytes[40]);
    if (!valid_side(side)) {
      return PayloadCodecStatus::InvalidField;
    }
    decoded.message = Event{OrderRestedEvent{
        read_u64(bytes, 24), read_u64(bytes, 32), static_cast<Side>(side),
        read_u32(bytes, 44), read_u32(bytes, 48)}};
    break;
  }
  case EventType::OrderDone:
    decoded.message = Event{OrderDoneEvent{read_u64(bytes, 24)}};
    break;
  case EventType::SaveSnapshot:
    decoded.message = Event{SaveSnapshotEvent{
        read_u64(bytes, 24), read_u64(bytes, 32)}};
    break;
  case EventType::LoadSnapshot:
    decoded.message = Event{LoadSnapshotEvent{
        read_u64(bytes, 24), read_u64(bytes, 32)}};
    break;
  case EventType::Shutdown:
    decoded.message = Event{ShutdownEvent{}};
    break;
  case EventType::MatcherFatal: {
    const std::uint8_t reason = static_cast<std::uint8_t>(bytes[24]);
    if (!valid_fatal_reason(reason)) {
      return PayloadCodecStatus::InvalidField;
    }
    decoded.message = Event{MatcherFatalEvent{
        static_cast<FatalReason>(reason), read_u64(bytes, 32),
        read_u64(bytes, 40)}};
    break;
  }
  case EventType::None:
    return PayloadCodecStatus::InvalidType;
  default:
    return PayloadCodecStatus::InvalidType;
  }

  std::array<std::byte, event_wal_payload_size_v1> canonical{};
  const PayloadCodecStatus encoded =
      encode_event_wal_payload_v1(decoded, canonical);
  if (encoded != PayloadCodecStatus::Ok) {
    return encoded;
  }
  if (!equals(bytes, canonical)) {
    return PayloadCodecStatus::NonCanonicalBytes;
  }

  payload = decoded;
  return PayloadCodecStatus::Ok;
}

} // namespace fexma::matcher
