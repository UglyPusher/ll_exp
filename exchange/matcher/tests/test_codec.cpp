/**
 * @file test_codec.cpp
 * @brief Golden-byte and validation tests for matcher WAL payload codecs.
 */
#include <fexma/matcher/codec.hpp>

#include <array>
#include <cstddef>
#include <cstdlib>

using namespace fexma::matcher;

namespace {

[[nodiscard]] bool command_golden_bytes() {
  const CommandWalPayload payload{
      0x0102030405060708ull,
      Command{NewLimitOrder{0x1112131415161718ull,
                            0x2122232425262728ull, Side::Ask,
                            0x31323334u, 0x41424344u}}};

  std::array<std::byte, command_wal_payload_size_v1> bytes{};
  if (encode_command_wal_payload_v1(payload, bytes) !=
      PayloadCodecStatus::Ok) {
    return false;
  }

  std::array<std::byte, command_wal_payload_size_v1> expected{};
  expected[0] = std::byte{0x08};
  expected[1] = std::byte{0x07};
  expected[2] = std::byte{0x06};
  expected[3] = std::byte{0x05};
  expected[4] = std::byte{0x04};
  expected[5] = std::byte{0x03};
  expected[6] = std::byte{0x02};
  expected[7] = std::byte{0x01};
  expected[8] = std::byte{0x01};
  expected[16] = std::byte{0x18};
  expected[17] = std::byte{0x17};
  expected[18] = std::byte{0x16};
  expected[19] = std::byte{0x15};
  expected[20] = std::byte{0x14};
  expected[21] = std::byte{0x13};
  expected[22] = std::byte{0x12};
  expected[23] = std::byte{0x11};
  expected[24] = std::byte{0x28};
  expected[25] = std::byte{0x27};
  expected[26] = std::byte{0x26};
  expected[27] = std::byte{0x25};
  expected[28] = std::byte{0x24};
  expected[29] = std::byte{0x23};
  expected[30] = std::byte{0x22};
  expected[31] = std::byte{0x21};
  expected[32] = std::byte{0x01};
  expected[36] = std::byte{0x34};
  expected[37] = std::byte{0x33};
  expected[38] = std::byte{0x32};
  expected[39] = std::byte{0x31};
  expected[40] = std::byte{0x44};
  expected[41] = std::byte{0x43};
  expected[42] = std::byte{0x42};
  expected[43] = std::byte{0x41};

  CommandWalPayload decoded{};
  return bytes == expected &&
         decode_command_wal_payload_v1(bytes, decoded) ==
             PayloadCodecStatus::Ok &&
         decoded.client_id == payload.client_id &&
         decoded.message.type == CommandType::NewLimit &&
         decoded.message.new_limit.id == payload.message.new_limit.id &&
         decoded.message.new_limit.owner_id ==
             payload.message.new_limit.owner_id &&
         decoded.message.new_limit.side == Side::Ask &&
         decoded.message.new_limit.price == payload.message.new_limit.price &&
         decoded.message.new_limit.quantity ==
             payload.message.new_limit.quantity;
}

[[nodiscard]] bool event_golden_bytes() {
  const EventWalPayload payload{
      0x0102030405060708ull, 0x1112131415161718ull, 0x21222324u, true,
      Event{TradeEvent{0x3132333435363738ull, 0x4142434445464748ull,
                       0x5152535455565758ull, 0x6162636465666768ull,
                       0x71727374u, 0x81828384u}}};

  std::array<std::byte, event_wal_payload_size_v1> bytes{};
  if (encode_event_wal_payload_v1(payload, bytes) != PayloadCodecStatus::Ok) {
    return false;
  }

  const std::array<std::byte, event_wal_payload_size_v1> expected{
      std::byte{0x08}, std::byte{0x07}, std::byte{0x06}, std::byte{0x05},
      std::byte{0x04}, std::byte{0x03}, std::byte{0x02}, std::byte{0x01},
      std::byte{0x18}, std::byte{0x17}, std::byte{0x16}, std::byte{0x15},
      std::byte{0x14}, std::byte{0x13}, std::byte{0x12}, std::byte{0x11},
      std::byte{0x24}, std::byte{0x23}, std::byte{0x22}, std::byte{0x21},
      std::byte{0x01}, std::byte{0x03}, std::byte{0x00}, std::byte{0x00},
      std::byte{0x38}, std::byte{0x37}, std::byte{0x36}, std::byte{0x35},
      std::byte{0x34}, std::byte{0x33}, std::byte{0x32}, std::byte{0x31},
      std::byte{0x48}, std::byte{0x47}, std::byte{0x46}, std::byte{0x45},
      std::byte{0x44}, std::byte{0x43}, std::byte{0x42}, std::byte{0x41},
      std::byte{0x58}, std::byte{0x57}, std::byte{0x56}, std::byte{0x55},
      std::byte{0x54}, std::byte{0x53}, std::byte{0x52}, std::byte{0x51},
      std::byte{0x68}, std::byte{0x67}, std::byte{0x66}, std::byte{0x65},
      std::byte{0x64}, std::byte{0x63}, std::byte{0x62}, std::byte{0x61},
      std::byte{0x74}, std::byte{0x73}, std::byte{0x72}, std::byte{0x71},
      std::byte{0x84}, std::byte{0x83}, std::byte{0x82}, std::byte{0x81}};

  EventWalPayload decoded{};
  return bytes == expected &&
         decode_event_wal_payload_v1(bytes, decoded) ==
             PayloadCodecStatus::Ok &&
         decoded.client_id == payload.client_id &&
         decoded.caused_by_command_sequence ==
             payload.caused_by_command_sequence &&
         decoded.index_in_command == payload.index_in_command &&
         decoded.is_last_for_command &&
         decoded.message.type == EventType::Trade &&
         decoded.message.trade.maker_owner_id ==
             payload.message.trade.maker_owner_id &&
         decoded.message.trade.quantity == payload.message.trade.quantity;
}

[[nodiscard]] bool command_variants_round_trip() {
  const std::array<CommandWalPayload, 4> payloads{
      CommandWalPayload{7, Command{NewLimitOrder{1, 2, Side::Bid, 3, 4}}},
      CommandWalPayload{7, Command{SaveSnapshotCommand{8, 9}}},
      CommandWalPayload{7, Command{LoadSnapshotCommand{10, 11}}},
      CommandWalPayload{7, Command{ShutdownCommand{}}}};

  for (const CommandWalPayload& payload : payloads) {
    std::array<std::byte, command_wal_payload_size_v1> bytes{};
    CommandWalPayload decoded{};
    if (encode_command_wal_payload_v1(payload, bytes) !=
            PayloadCodecStatus::Ok ||
        decode_command_wal_payload_v1(bytes, decoded) !=
            PayloadCodecStatus::Ok ||
        decoded.client_id != payload.client_id ||
        decoded.message.type != payload.message.type) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] bool event_variants_round_trip() {
  const std::array<Event, 9> messages{
      Event{OrderAcceptedEvent{1}},
      Event{OrderRejectedEvent{2, RejectReason::InvalidQuantity}},
      Event{TradeEvent{1, 2, 3, 4, 5, 6}},
      Event{OrderRestedEvent{1, 2, Side::Ask, 3, 4}},
      Event{OrderDoneEvent{1}},
      Event{SaveSnapshotEvent{1, 2}},
      Event{LoadSnapshotEvent{3, 4}},
      Event{ShutdownEvent{}},
      Event{MatcherFatalEvent{FatalReason::NonMonotonicOrderId, 5, 4}}};

  for (const Event& message : messages) {
    const EventWalPayload payload{7, 8, 9, true, message};
    std::array<std::byte, event_wal_payload_size_v1> bytes{};
    EventWalPayload decoded{};
    if (encode_event_wal_payload_v1(payload, bytes) !=
            PayloadCodecStatus::Ok ||
        decode_event_wal_payload_v1(bytes, decoded) !=
            PayloadCodecStatus::Ok ||
        decoded.client_id != payload.client_id ||
        decoded.message.type != payload.message.type) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] bool replay_commands_use_schema_v2() {
  const CommandWalPayload start{
      0x0102030405060708ull,
      Command{StartReplayCommand{0x1112131415161718ull,
                                 0x2122232425262728ull,
                                 0x3132333435363738ull,
                                 0x4142434445464748ull}}};
  std::array<std::byte, command_wal_payload_size_v2> bytes{};
  if (current_command_schema_version != 2 ||
      encode_command_wal_payload_v1(start, bytes) !=
          PayloadCodecStatus::InvalidType ||
      encode_command_wal_payload_v2(start, bytes) != PayloadCodecStatus::Ok ||
      bytes[8] != static_cast<std::byte>(CommandType::StartReplay) ||
      bytes[16] != std::byte{0x18} || bytes[24] != std::byte{0x28} ||
      bytes[32] != std::byte{0x38} || bytes[40] != std::byte{0x48}) {
    return false;
  }

  CommandWalPayload decoded{};
  if (decode_command_wal_payload_v1(bytes, decoded) !=
          PayloadCodecStatus::InvalidType ||
      decode_command_wal_payload_v2(bytes, decoded) !=
          PayloadCodecStatus::Ok ||
      decoded.message.type != CommandType::StartReplay ||
      decoded.message.start_replay.replay_id !=
          start.message.start_replay.replay_id ||
      decoded.message.start_replay.live_snapshot_id !=
          start.message.start_replay.live_snapshot_id ||
      decoded.message.start_replay.replay_snapshot_id !=
          start.message.start_replay.replay_snapshot_id ||
      decoded.message.start_replay.replay_through_command_sequence !=
          start.message.start_replay.replay_through_command_sequence) {
    return false;
  }

  const CommandWalPayload stop{7, Command{StopReplayCommand{9}}};
  if (encode_command_wal_payload_v2(stop, bytes) != PayloadCodecStatus::Ok ||
      decode_command_wal_payload_v2(bytes, decoded) !=
          PayloadCodecStatus::Ok ||
      decoded.message.type != CommandType::StopReplay ||
      decoded.message.stop_replay.replay_id != 9) {
    return false;
  }
  bytes[47] = std::byte{1};
  return decode_command_wal_payload_v2(bytes, decoded) ==
         PayloadCodecStatus::NonCanonicalBytes;
}

[[nodiscard]] bool replay_events_use_schema_v2() {
  const EventWalPayload start{
      1, 2, 3, true,
      Event{StartReplayEvent{4, 5, 6, 7}}};
  std::array<std::byte, event_wal_payload_size_v2> bytes{};
  if (current_event_schema_version != 2 ||
      encode_event_wal_payload_v1(start, bytes) !=
          PayloadCodecStatus::InvalidType ||
      encode_event_wal_payload_v2(start, bytes) != PayloadCodecStatus::Ok ||
      bytes[21] != static_cast<std::byte>(EventType::StartReplay)) {
    return false;
  }

  EventWalPayload decoded{};
  if (decode_event_wal_payload_v1(bytes, decoded) !=
          PayloadCodecStatus::InvalidType ||
      decode_event_wal_payload_v2(bytes, decoded) != PayloadCodecStatus::Ok ||
      decoded.message.type != EventType::StartReplay ||
      decoded.message.start_replay.replay_id != 4 ||
      decoded.message.start_replay.live_snapshot_id != 5 ||
      decoded.message.start_replay.replay_snapshot_id != 6 ||
      decoded.message.start_replay.replay_through_command_sequence != 7) {
    return false;
  }

  const EventWalPayload stop{8, 9, 0, true,
                             Event{StopReplayEvent{10}}};
  return encode_event_wal_payload_v2(stop, bytes) ==
             PayloadCodecStatus::Ok &&
         decode_event_wal_payload_v2(bytes, decoded) ==
             PayloadCodecStatus::Ok &&
         decoded.message.type == EventType::StopReplay &&
         decoded.message.stop_replay.replay_id == 10;
}

[[nodiscard]] bool schema_v2_preserves_schema_v1_bytes() {
  const CommandWalPayload command{
      7, Command{NewLimitOrder{1, 2, Side::Bid, 3, 4}}};
  std::array<std::byte, command_wal_payload_size_v1> command_v1{};
  std::array<std::byte, command_wal_payload_size_v2> command_v2{};
  const EventWalPayload event{
      7, 8, 0, true, Event{OrderDoneEvent{9}}};
  std::array<std::byte, event_wal_payload_size_v1> event_v1{};
  std::array<std::byte, event_wal_payload_size_v2> event_v2{};
  return encode_command_wal_payload_v1(command, command_v1) ==
             PayloadCodecStatus::Ok &&
         encode_command_wal_payload_v2(command, command_v2) ==
             PayloadCodecStatus::Ok &&
         command_v1 == command_v2 &&
         encode_event_wal_payload_v1(event, event_v1) ==
             PayloadCodecStatus::Ok &&
         encode_event_wal_payload_v2(event, event_v2) ==
             PayloadCodecStatus::Ok &&
         event_v1 == event_v2;
}

[[nodiscard]] bool rejects_invalid_and_noncanonical_bytes() {
  std::array<std::byte, command_wal_payload_size_v1> command{};
  command[8] = std::byte{0xff};
  CommandWalPayload decoded_command{};
  if (decode_command_wal_payload_v1(command, decoded_command) !=
      PayloadCodecStatus::InvalidType) {
    return false;
  }

  command.fill(std::byte{});
  command[8] = static_cast<std::byte>(CommandType::Shutdown);
  command[47] = std::byte{1};
  if (decode_command_wal_payload_v1(command, decoded_command) !=
      PayloadCodecStatus::NonCanonicalBytes) {
    return false;
  }

  std::array<std::byte, event_wal_payload_size_v1> event{};
  event[20] = std::byte{2};
  EventWalPayload decoded_event{};
  if (decode_event_wal_payload_v1(event, decoded_event) !=
      PayloadCodecStatus::InvalidField) {
    return false;
  }

  event.fill(std::byte{});
  event[20] = std::byte{1};
  event[21] = static_cast<std::byte>(EventType::Shutdown);
  event[63] = std::byte{1};
  return decode_event_wal_payload_v1(event, decoded_event) ==
         PayloadCodecStatus::NonCanonicalBytes;
}

[[nodiscard]] bool rejects_wrong_sizes_and_invalid_domain_values() {
  const CommandWalPayload invalid_command{
      1, Command{NewLimitOrder{1, 2, static_cast<Side>(9), 3, 4}}};
  std::array<std::byte, command_wal_payload_size_v1> command{};
  if (encode_command_wal_payload_v1(invalid_command, command) !=
      PayloadCodecStatus::InvalidField) {
    return false;
  }

  const EventWalPayload invalid_event{
      1, 2, 3, true,
      Event{OrderRejectedEvent{4, static_cast<RejectReason>(0xff)}}};
  std::array<std::byte, event_wal_payload_size_v1> event{};
  if (encode_event_wal_payload_v1(invalid_event, event) !=
      PayloadCodecStatus::InvalidField) {
    return false;
  }

  EventWalPayload decoded_event{};
  return encode_command_wal_payload_v1(
             CommandWalPayload{1, Command{ShutdownCommand{}}},
             std::span<std::byte>(command).first(command.size() - 1)) ==
             PayloadCodecStatus::InvalidSize &&
         decode_event_wal_payload_v1(
             std::span<const std::byte>(event).first(event.size() - 1),
             decoded_event) == PayloadCodecStatus::InvalidSize;
}

} // namespace

int main() {
  if (!command_golden_bytes()) {
    return EXIT_FAILURE;
  }
  if (!event_golden_bytes()) {
    return EXIT_FAILURE;
  }
  if (!command_variants_round_trip()) {
    return EXIT_FAILURE;
  }
  if (!event_variants_round_trip()) {
    return EXIT_FAILURE;
  }
  if (!replay_commands_use_schema_v2()) {
    return EXIT_FAILURE;
  }
  if (!replay_events_use_schema_v2()) {
    return EXIT_FAILURE;
  }
  if (!schema_v2_preserves_schema_v1_bytes()) {
    return EXIT_FAILURE;
  }
  if (!rejects_invalid_and_noncanonical_bytes()) {
    return EXIT_FAILURE;
  }
  if (!rejects_wrong_sizes_and_invalid_domain_values()) {
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
