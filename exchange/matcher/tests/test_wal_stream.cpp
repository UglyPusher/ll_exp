/**
 * @file test_wal_stream.cpp
 * @brief End-to-end canonical matcher payload tests through the physical WAL.
 */
#include <fexma/matcher/codec.hpp>
#include <fexma/matcher/matcher.hpp>
#include <fexma/matcher/replay.hpp>
#include <fexma/wal/persistence.hpp>
#include <fexma/wal/reader.hpp>

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

[[nodiscard]] fexma::wal::PhysicalWalConfig
physical_config(const fexma::wal::WalConfig& config) noexcept {
  return {config.payload_size,
          config.alignment,
          config.payload_schema_version,
          config.stream_kind,
          config.stream_id,
          config.epoch_id,
          config.first_sequence,
          config.manifest_id};
}

[[nodiscard]] bool write_command_stream(const std::filesystem::path& path) {
  std::filesystem::remove(path);
  const auto config = command_config();
  fexma::wal::PersistenceModule persistence;
  if (!persistence.open(path, physical_config(config)).ok()) {
    return false;
  }

  const std::array<CommandWalPayload, 4> commands{
      CommandWalPayload{17,
                        Command{NewLimitOrder{1, 11, Side::Bid, 150, 10}}},
      CommandWalPayload{17, Command{SaveSnapshotCommand{}}},
      CommandWalPayload{17, Command{StartReplayCommand{}}},
      CommandWalPayload{17, Command{ShutdownCommand{}}}};
  std::array<std::byte, command_wal_payload_size_v2> bytes{};

  for (std::size_t index = 0; index < commands.size(); ++index) {
    const CommandWalPayload& command = commands[index];
    if (encode_command_wal_payload_v2(command, bytes) !=
            PayloadCodecStatus::Ok ||
        !persistence.append(
            {index, config.first_sequence + index, bytes})) {
      return false;
    }
  }
  return persistence.sync() && persistence.close();
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
  const auto config = event_config();
  fexma::wal::PersistenceModule persistence;
  if (!persistence.open(path, physical_config(config)).ok()) {
    return false;
  }

  const std::array<EventWalPayload, 3> events{
      EventWalPayload{17, 1, 0, false, Event{OrderAcceptedEvent{1}}},
      EventWalPayload{17, 1, 1, true,
                      Event{OrderRestedEvent{1, 11, Side::Bid, 150, 10}}},
      EventWalPayload{17, 3, 0, true,
                      Event{StartReplayEvent{}}}};
  std::array<std::byte, event_wal_payload_size_v2> bytes{};

  for (std::size_t index = 0; index < events.size(); ++index) {
    const EventWalPayload& event = events[index];
    if (encode_event_wal_payload_v2(event, bytes) !=
            PayloadCodecStatus::Ok ||
        !persistence.append(
            {index, config.first_sequence + index, bytes})) {
      return false;
    }
  }
  return persistence.sync() && persistence.close();
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
    const std::filesystem::path& event_path,
    MatcherStateCheckpoint& checkpoint) {
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

  std::array<fexma::order_book::OrderView, 8> scratch{};
  return slot.command.command_sequence == 1 &&
         slot.risk.decision == CheckDecision::Accepted &&
         slot.reserve.decision == CheckDecision::Accepted &&
         matcher.book().order_count() == 1 &&
         make_matcher_state_checkpoint(
             matcher.book(), OrderBookConfig{100, 200, 8},
             matcher.last_order_id(), matcher.next_event_sequence(), scratch,
             checkpoint).ok();
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

[[nodiscard]] bool replay_fsm_restores_live_cursor() {
  CommandPipeline pipeline;
  ReplayPersistenceState state;
  if (pipeline.open({8, 100}) != CommandPipelineStatus::Ok) {
    return false;
  }
  const CommandPipelineResult start = pipeline.try_publish(
      {17, Command{StartReplayCommand{}}});
  CommandEnvelope command{};
  if (start.sequence != 100 ||
      state.action() != PersistenceAction::AppendAndSync ||
      !pipeline.copy_pending_for_persistence(0, command).ok() ||
      !pipeline.publish_durable(1).ok() ||
      state.on_durable(command, pipeline) != ReplayStatus::Ok ||
      state.mode() != ReplayMode::Replay ||
      state.live_resume_sequence() != 101 ||
      state.action() != PersistenceAction::AdvanceDurableOnly) {
    return false;
  }

  if (!pipeline.try_replay(
          {1, {17, Command{NewLimitOrder{1, 1, Side::Bid, 150, 1}}}}).ok() ||
      !pipeline.publish_durable(1).ok()) {
    return false;
  }
  const CommandPipelineResult stop =
      pipeline.try_publish({17, Command{StopReplayCommand{}}});
  if (stop.sequence != 101 || !pipeline.publish_durable(1).ok() ||
      state.on_durable({101, {17, Command{StopReplayCommand{}}}}, pipeline) !=
          ReplayStatus::Ok ||
      state.mode() != ReplayMode::Restoring) {
    return false;
  }

  const CommandPipelineResult restore = pipeline.try_publish(
      {17, Command{LoadSnapshotCommand{8, 7}}});
  if (restore.sequence != 102 || !pipeline.publish_durable(1).ok() ||
      state.on_durable(
          {102, {17, Command{LoadSnapshotCommand{8, 7}}}}, pipeline) !=
          ReplayStatus::Ok ||
      state.mode() != ReplayMode::Live) {
    return false;
  }
  return pipeline.try_publish({17, Command{ShutdownCommand{}}}).sequence ==
         101;
}

[[nodiscard]] bool validates_manifest_and_config_hash() {
  const OrderBookConfig book{100, 200, 8};
  const ReplayManifest manifest{1001, 7, 101, 201, 2, 2,
                                hash_order_book_config(book)};
  if (validate_replay_manifest(manifest, command_config(), event_config(),
                               book) != ReplayStatus::Ok) {
    return false;
  }
  ReplayManifest wrong = manifest;
  ++wrong.configuration_hash;
  return validate_replay_manifest(wrong, command_config(), event_config(),
                                  book) == ReplayStatus::ManifestMismatch;
}

[[nodiscard]] bool matcher_checkpoint_is_canonical_and_bounded() {
  UnusedCommandReader reader;
  EventWalComparator unused_writer;
  Matcher matcher(reader, unused_writer, OrderBookConfig{100, 200, 8});
  std::array<fexma::order_book::OrderView, 8> scratch{};
  std::array<fexma::order_book::OrderView, 0> no_capacity{};
  MatcherStateCheckpoint empty{};
  MatcherStateCheckpoint changed{};

  if (!make_matcher_state_checkpoint(
           matcher.book(), OrderBookConfig{100, 200, 8},
           matcher.last_order_id(), matcher.next_event_sequence(), scratch,
           empty).ok()) {
    return false;
  }

  fexma::order_book::OrderBook book(OrderBookConfig{100, 200, 8});
  if (!book.insert({1, 11, Side::Bid, 150, 10}).ok() ||
      !make_matcher_state_checkpoint(
           book, OrderBookConfig{100, 200, 8}, 1, 3, scratch, changed).ok()) {
    return false;
  }
  MatcherStateCheckpoint rejected{};
  const ReplayResult bounded = make_matcher_state_checkpoint(
      book, OrderBookConfig{100, 200, 8}, 1, 3, no_capacity, rejected);
  return empty.canonical_hash != changed.canonical_hash &&
         changed.order_count == 1 &&
         bounded.status == ReplayStatus::CheckpointCapacityExceeded;
}

} // namespace

int main() {
  const std::filesystem::path command_path =
      test_path("fexma_matcher_command_stream.wal");
  const std::filesystem::path event_path =
      test_path("fexma_matcher_event_stream.wal");

  MatcherStateCheckpoint first{};
  MatcherStateCheckpoint second{};
  const bool passed = write_command_stream(command_path) &&
                      read_command_stream(command_path) &&
                      write_event_stream(event_path) &&
                      read_event_stream(event_path) &&
                      replay_first_command(command_path, event_path, first) &&
                      replay_first_command(command_path, event_path, second) &&
                      first.canonical_hash == second.canonical_hash &&
                      first.order_count == second.order_count &&
                      detects_first_event_mismatch(event_path) &&
                      replay_fsm_restores_live_cursor() &&
                      validates_manifest_and_config_hash() &&
                      matcher_checkpoint_is_canonical_and_bounded();
  std::filesystem::remove(command_path);
  std::filesystem::remove(event_path);
  return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
