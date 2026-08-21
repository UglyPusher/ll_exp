/**
 * @file test_wal_stream.cpp
 * @brief End-to-end canonical matcher payload tests through the physical WAL.
 */
#include <fexma/matcher/codec.hpp>
#include <fexma/wal/reader.hpp>
#include <fexma/wal/wal.hpp>

#include <array>
#include <cstddef>
#include <cstdlib>
#include <filesystem>

using namespace fexma::matcher;

namespace {

[[nodiscard]] std::filesystem::path test_path(const char* name) {
  return std::filesystem::temp_directory_path() / name;
}

[[nodiscard]] fexma::wal::WalConfig command_config() noexcept {
  return {static_cast<std::uint32_t>(command_wal_payload_size_v1),
          4,
          fexma::wal::default_alignment,
          current_command_schema_version,
          fexma::wal::StreamKind::Command,
          101,
          7,
          1,
          1001};
}

[[nodiscard]] fexma::wal::WalConfig event_config() noexcept {
  return {static_cast<std::uint32_t>(event_wal_payload_size_v1),
          4,
          fexma::wal::default_alignment,
          current_event_schema_version,
          fexma::wal::StreamKind::Event,
          201,
          7,
          1,
          1001};
}

[[nodiscard]] bool write_command_stream(const std::filesystem::path& path) {
  std::filesystem::remove(path);
  fexma::wal::Wal wal;
  if (!wal.open(path, command_config()).ok()) {
    return false;
  }

  const std::array<CommandWalPayload, 3> commands{
      CommandWalPayload{17,
                        Command{NewLimitOrder{1, 11, Side::Bid, 150, 10}}},
      CommandWalPayload{17, Command{SaveSnapshotCommand{5, 7}}},
      CommandWalPayload{17, Command{ShutdownCommand{}}}};
  std::array<std::byte, command_wal_payload_size_v1> bytes{};

  for (const CommandWalPayload& command : commands) {
    if (encode_command_wal_payload_v1(command, bytes) !=
            PayloadCodecStatus::Ok ||
        !wal.try_publish(bytes).ok()) {
      return false;
    }
  }
  const fexma::wal::DurabilityResult durable = wal.advance_durable(3);
  if (!durable.ok() || durable.records != commands.size()) {
    return false;
  }
  for (std::size_t index = 0; index < commands.size(); ++index) {
    if (!wal.try_consume(bytes).ok()) {
      return false;
    }
  }
  return wal.close().ok();
}

[[nodiscard]] bool read_command_stream(const std::filesystem::path& path) {
  fexma::wal::WalReader reader;
  if (!reader.open(path, command_config()).ok()) {
    return false;
  }

  const std::array<CommandType, 3> expected_types{
      CommandType::NewLimit, CommandType::SaveSnapshot,
      CommandType::Shutdown};
  std::array<std::byte, command_wal_payload_size_v1> bytes{};
  for (std::size_t index = 0; index < expected_types.size(); ++index) {
    const fexma::wal::ReadResult read = reader.read_next(bytes);
    CommandWalPayload payload{};
    if (!read.ok() || read.sequence != index + 1 ||
        decode_command_wal_payload_v1(bytes, payload) !=
            PayloadCodecStatus::Ok ||
        payload.client_id != 17 ||
        payload.message.type != expected_types[index]) {
      return false;
    }
  }
  return reader.read_next(bytes).status == fexma::wal::ReadStatus::EndOfLog;
}

[[nodiscard]] bool write_event_stream(const std::filesystem::path& path) {
  std::filesystem::remove(path);
  fexma::wal::Wal wal;
  if (!wal.open(path, event_config()).ok()) {
    return false;
  }

  const std::array<EventWalPayload, 2> events{
      EventWalPayload{17, 1, 0, false, Event{OrderAcceptedEvent{1}}},
      EventWalPayload{17, 1, 1, true,
                      Event{OrderRestedEvent{1, 11, Side::Bid, 150, 10}}}};
  std::array<std::byte, event_wal_payload_size_v1> bytes{};

  for (const EventWalPayload& event : events) {
    if (encode_event_wal_payload_v1(event, bytes) !=
            PayloadCodecStatus::Ok ||
        !wal.try_publish(bytes).ok()) {
      return false;
    }
  }
  const fexma::wal::DurabilityResult durable = wal.advance_durable(2);
  if (!durable.ok() || durable.records != events.size()) {
    return false;
  }
  for (std::size_t index = 0; index < events.size(); ++index) {
    if (!wal.try_consume(bytes).ok()) {
      return false;
    }
  }
  return wal.close().ok();
}

[[nodiscard]] bool read_event_stream(const std::filesystem::path& path) {
  fexma::wal::WalReader reader;
  if (!reader.open(path, event_config()).ok()) {
    return false;
  }

  const std::array<EventType, 2> expected_types{
      EventType::OrderAccepted, EventType::OrderRested};
  std::array<std::byte, event_wal_payload_size_v1> bytes{};
  for (std::size_t index = 0; index < expected_types.size(); ++index) {
    const fexma::wal::ReadResult read = reader.read_next(bytes);
    EventWalPayload payload{};
    if (!read.ok() || read.sequence != index + 1 ||
        decode_event_wal_payload_v1(bytes, payload) !=
            PayloadCodecStatus::Ok ||
        payload.caused_by_command_sequence != 1 ||
        payload.index_in_command != index ||
        payload.is_last_for_command != (index + 1 == expected_types.size()) ||
        payload.message.type != expected_types[index]) {
      return false;
    }
  }
  return reader.read_next(bytes).status == fexma::wal::ReadStatus::EndOfLog;
}

} // namespace

int main() {
  const std::filesystem::path command_path =
      test_path("fexma_matcher_command_stream.wal");
  const std::filesystem::path event_path =
      test_path("fexma_matcher_event_stream.wal");

  const bool passed = write_command_stream(command_path) &&
                      read_command_stream(command_path) &&
                      write_event_stream(event_path) &&
                      read_event_stream(event_path);
  std::filesystem::remove(command_path);
  std::filesystem::remove(event_path);
  return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
