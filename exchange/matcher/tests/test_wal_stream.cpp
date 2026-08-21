/**
 * @file test_wal_stream.cpp
 * @brief End-to-end canonical matcher payload tests through the physical WAL.
 */
#include <fexma/matcher/codec.hpp>
#include <fexma/matcher/matcher.hpp>
#include <fexma/matcher/replay.hpp>
#include <fexma/wal/reader.hpp>
#include <fexma/wal/wal.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>

using namespace fexma::matcher;

namespace {

class UnusedCommandReader {
public:
  [[nodiscard]] CommandReadResult read_next() noexcept {
    return {CommandReadStatus::Empty, {}};
  }
};

[[nodiscard]] std::filesystem::path test_path(const char* name) {
  return std::filesystem::temp_directory_path() / name;
}

[[nodiscard]] fexma::wal::WalConfig command_config() noexcept {
  return {static_cast<std::uint32_t>(command_wal_payload_size_v2),
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
  return {static_cast<std::uint32_t>(event_wal_payload_size_v2),
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

  const std::array<CommandWalPayload, 4> commands{
      CommandWalPayload{17,
                        Command{NewLimitOrder{1, 11, Side::Bid, 150, 10}}},
      CommandWalPayload{17, Command{SaveSnapshotCommand{5, 7}}},
      CommandWalPayload{17,
                        Command{StartReplayCommand{9, 5, 3, 2}}},
      CommandWalPayload{17, Command{ShutdownCommand{}}}};
  std::array<std::byte, command_wal_payload_size_v2> bytes{};

  for (const CommandWalPayload& command : commands) {
    if (encode_command_wal_payload_v2(command, bytes) !=
            PayloadCodecStatus::Ok ||
        !wal.try_publish(bytes).ok()) {
      return false;
    }
  }
  const fexma::wal::DurabilityResult durable =
      wal.advance_durable(static_cast<std::uint32_t>(commands.size()));
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

  const std::array<CommandType, 4> expected_types{
      CommandType::NewLimit, CommandType::SaveSnapshot,
      CommandType::StartReplay, CommandType::Shutdown};
  std::array<std::byte, command_wal_payload_size_v2> bytes{};
  for (std::size_t index = 0; index < expected_types.size(); ++index) {
    const fexma::wal::ReadResult read = reader.read_next(bytes);
    CommandWalPayload payload{};
    if (!read.ok() || read.sequence != index + 1 ||
        decode_command_wal_payload_v2(bytes, payload) !=
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

  const std::array<EventWalPayload, 3> events{
      EventWalPayload{17, 1, 0, false, Event{OrderAcceptedEvent{1}}},
      EventWalPayload{17, 1, 1, true,
                      Event{OrderRestedEvent{1, 11, Side::Bid, 150, 10}}},
      EventWalPayload{17, 3, 0, true,
                      Event{StartReplayEvent{9, 5, 3, 2}}}};
  std::array<std::byte, event_wal_payload_size_v2> bytes{};

  for (const EventWalPayload& event : events) {
    if (encode_event_wal_payload_v2(event, bytes) !=
            PayloadCodecStatus::Ok ||
        !wal.try_publish(bytes).ok()) {
      return false;
    }
  }
  const fexma::wal::DurabilityResult durable =
      wal.advance_durable(static_cast<std::uint32_t>(events.size()));
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

  const std::array<EventType, 3> expected_types{
      EventType::OrderAccepted, EventType::OrderRested,
      EventType::StartReplay};
  const std::array<CommandSequence, 3> expected_causes{1, 1, 3};
  const std::array<EventIndex, 3> expected_indices{0, 1, 0};
  const std::array<bool, 3> expected_final{false, true, true};
  std::array<std::byte, event_wal_payload_size_v2> bytes{};
  for (std::size_t index = 0; index < expected_types.size(); ++index) {
    const fexma::wal::ReadResult read = reader.read_next(bytes);
    EventWalPayload payload{};
    if (!read.ok() || read.sequence != index + 1 ||
        decode_event_wal_payload_v2(bytes, payload) !=
            PayloadCodecStatus::Ok ||
        payload.caused_by_command_sequence != expected_causes[index] ||
        payload.index_in_command != expected_indices[index] ||
        payload.is_last_for_command != expected_final[index] ||
        payload.message.type != expected_types[index]) {
      return false;
    }
  }
  return reader.read_next(bytes).status == fexma::wal::ReadStatus::EndOfLog;
}

[[nodiscard]] bool replay_first_command(
    const std::filesystem::path& command_path,
    const std::filesystem::path& event_path) {
  CommandPipeline pipeline;
  WalFileReplaySource source;
  EventWalComparator comparator;
  UnusedCommandReader unused_reader;
  Matcher matcher(unused_reader, comparator, OrderBookConfig{100, 200, 8}, 1);
  if (pipeline.open({4, 100}) != CommandPipelineStatus::Ok ||
      source.open(command_path, command_config(), 1, 1) != ReplayStatus::Ok ||
      comparator.open(event_path, event_config(), 1, 2) != ReplayStatus::Ok ||
      !source.publish_next(pipeline).ok()) {
    return false;
  }

  CommandEnvelope command{};
  RiskResult risk{};
  CommandRingSlot slot{};
  if (!pipeline.publish_durable(1).ok() ||
      !pipeline.try_read_for_risk(command).ok() ||
      !pipeline.publish_risk({CheckDecision::Accepted, 1}).ok() ||
      !pipeline.try_read_for_reserve(command, risk).ok() ||
      !pipeline.publish_reserve({CheckDecision::Accepted, 2}).ok() ||
      !pipeline.try_consume(slot).ok() || matcher.process(slot.command).fatal() ||
      comparator.finish() != ReplayStatus::Complete) {
    return false;
  }

  return slot.command.command_sequence == 1 &&
         slot.risk.decision == CheckDecision::Accepted &&
         slot.reserve.decision == CheckDecision::Accepted &&
         matcher.book().order_count() == 1;
}

[[nodiscard]] bool detects_first_event_mismatch(
    const std::filesystem::path& event_path) {
  EventWalComparator comparator;
  if (comparator.open(event_path, event_config(), 1, 1) != ReplayStatus::Ok) {
    return false;
  }
  const EventEnvelope wrong{
      1, {17, 1, 0, true, Event{OrderAcceptedEvent{999}}}};
  return !comparator.publish(wrong).ok() &&
         comparator.result().status == ReplayStatus::EventMismatch &&
         comparator.result().sequence == 1;
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
                      read_event_stream(event_path) &&
                      replay_first_command(command_path, event_path) &&
                      replay_first_command(command_path, event_path) &&
                      detects_first_event_mismatch(event_path);
  std::filesystem::remove(command_path);
  std::filesystem::remove(event_path);
  return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
