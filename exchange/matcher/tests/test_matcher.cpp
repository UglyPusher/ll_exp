/**
 * @file test_matcher.cpp
 * @brief Contract tests for the minimal matcher sample.
 */
#include <fexma/matcher/matcher.hpp>

#include <array>
#include <cstddef>
#include <cstdlib>
#include <vector>

using namespace fexma::matcher;

namespace {

inline constexpr ClientId test_client_id = 17;

[[nodiscard]] CommandEnvelope envelope(CommandSequence command_sequence,
                                       Command message) noexcept {
  return {command_sequence, {test_client_id, message}};
}

class ShutdownCommandReader {
public:
  [[nodiscard]] CommandReadResult read_next() noexcept {
    return {CommandReadStatus::Ok,
            envelope(1, Command{ShutdownCommand{}})};
  }
};

class FatalCommandReader {
public:
  [[nodiscard]] CommandReadResult read_next() noexcept {
    return {CommandReadStatus::Fatal, {}};
  }
};

class CollectingEventWriter {
public:
  [[nodiscard]] PublishResult publish(const EventEnvelope& event) noexcept {
    if (fail_after != no_failure && event_count >= fail_after) {
      return {PublishStatus::Fatal};
    }
    if (event_count == events.size()) {
      return {PublishStatus::Fatal};
    }
    events[event_count++] = event;
    return {PublishStatus::Ok};
  }

  static constexpr std::size_t no_failure =
      static_cast<std::size_t>(-1);

  std::array<EventEnvelope, 64> events{};
  std::size_t event_count{};
  std::size_t fail_after{no_failure};
};

class MemorySnapshotStore {
public:
  [[nodiscard]] SnapshotOperationResult
  capture(const SaveSnapshotCommand& command,
          const MatcherSnapshotView& snapshot) {
    if (fail_capture) {
      return {SnapshotOperationStatus::Fatal};
    }
    if (snapshot.book == nullptr) {
      return {SnapshotOperationStatus::Invalid};
    }

    image.snapshot_id = command.snapshot_id;
    image.command_sequence = snapshot.command_sequence;
    image.epoch_id = command.snapshot_epoch_id;
    image.last_order_id = snapshot.last_order_id;
    image.book_config = snapshot.book_config;
    orders.resize(snapshot.book->order_count());

    const fexma::order_book::SnapshotResult copied =
        snapshot.book->snapshot_into(
            std::span<fexma::order_book::OrderView>(orders));
    if (!copied.ok()) {
      return {SnapshotOperationStatus::CapacityExceeded};
    }

    ++capture_count;
    return {SnapshotOperationStatus::Ok};
  }

  [[nodiscard]] SnapshotOperationResult
  load(const LoadSnapshotCommand& command, MatcherSnapshotImage& out) noexcept {
    if (fail_load) {
      return {SnapshotOperationStatus::Unavailable};
    }
    if (image.snapshot_id != command.snapshot_id ||
        image.epoch_id != command.snapshot_epoch_id) {
      return {SnapshotOperationStatus::Unavailable};
    }

    out = image;
    out.orders = std::span<const fexma::order_book::OrderView>(orders);
    ++load_count;
    return {SnapshotOperationStatus::Ok};
  }

  MatcherSnapshotImage image{};
  std::vector<fexma::order_book::OrderView> orders;
  std::uint32_t capture_count{};
  std::uint32_t load_count{};
  bool fail_capture{false};
  bool fail_load{false};
};

[[nodiscard]] CommandEnvelope new_limit(CommandSequence command_sequence,
                                        OrderId id, OwnerId owner_id,
                                        Side side, PriceTick price,
                                        Quantity quantity) noexcept {
  return envelope(command_sequence,
                  Command{NewLimitOrder{id, owner_id, side, price, quantity}});
}

[[nodiscard]] CommandEnvelope save_snapshot(CommandSequence command_sequence,
                                            SnapshotId snapshot_id,
                                            EpochId snapshot_epoch_id) noexcept {
  return envelope(
      command_sequence,
      Command{SaveSnapshotCommand{snapshot_id, snapshot_epoch_id}});
}

[[nodiscard]] CommandEnvelope load_snapshot(CommandSequence command_sequence,
                                            SnapshotId snapshot_id,
                                            EpochId snapshot_epoch_id) noexcept {
  return envelope(
      command_sequence,
      Command{LoadSnapshotCommand{snapshot_id, snapshot_epoch_id}});
}

[[nodiscard]] bool replay_commands_are_forwarded() {
  ShutdownCommandReader reader;
  CollectingEventWriter writer;
  Matcher matcher(reader, writer, OrderBookConfig{100, 200, 8});
  const CommandEnvelope start = envelope(
      1, Command{StartReplayCommand{9, 8, 7, 6}});
  const CommandEnvelope stop = envelope(2, Command{StopReplayCommand{9}});
  if (matcher.process(start).fatal() || matcher.process(stop).fatal() ||
      writer.event_count != 2) {
    return false;
  }
  const EventEnvelope& started = writer.events[0];
  const EventEnvelope& stopped = writer.events[1];
  return started.event_sequence == 1 &&
         started.payload.caused_by_command_sequence == 1 &&
         started.payload.message.type == EventType::StartReplay &&
         started.payload.message.start_replay.replay_id == 9 &&
         started.payload.message.start_replay.live_snapshot_id == 8 &&
         started.payload.message.start_replay.replay_snapshot_id == 7 &&
         started.payload.message.start_replay
                 .replay_through_command_sequence == 6 &&
         stopped.event_sequence == 2 &&
         stopped.payload.caused_by_command_sequence == 2 &&
         stopped.payload.message.type == EventType::StopReplay &&
         stopped.payload.message.stop_replay.replay_id == 9;
}

[[nodiscard]] bool event_type_is(const CollectingEventWriter& writer,
                                 std::size_t index,
                                 EventType type) noexcept {
  return index < writer.event_count &&
         writer.events[index].payload.message.type == type;
}

[[nodiscard]] bool event_metadata_is(
    const CollectingEventWriter& writer, std::size_t index,
    EventSequence event_sequence, CommandSequence command_sequence,
    EventIndex index_in_command, bool is_last_for_command) noexcept {
  if (index >= writer.event_count) {
    return false;
  }
  const EventEnvelope& event = writer.events[index];
  return event.event_sequence == event_sequence &&
         event.payload.client_id == test_client_id &&
         event.payload.caused_by_command_sequence == command_sequence &&
         event.payload.index_in_command == index_in_command &&
         event.payload.is_last_for_command == is_last_for_command;
}

[[nodiscard]] bool rests_non_crossing_limit() {
  ShutdownCommandReader reader;
  CollectingEventWriter writer;
  Matcher matcher(reader, writer, OrderBookConfig{100, 200, 8});

  const ProcessResult processed =
      matcher.process(new_limit(1, 1, 101, Side::Bid, 150, 10));
  const auto best_bid = matcher.book().best(Side::Bid);

  return processed.status == ProcessStatus::Continue && best_bid &&
         best_bid->id == 1 && best_bid->remaining == 10 &&
         writer.event_count == 2 &&
         event_type_is(writer, 0, EventType::OrderAccepted) &&
         event_type_is(writer, 1, EventType::OrderRested) &&
         event_metadata_is(writer, 0, 1, 1, 0, false) &&
         event_metadata_is(writer, 1, 2, 1, 1, true);
}

[[nodiscard]] bool matches_fifo_at_resting_price() {
  ShutdownCommandReader reader;
  CollectingEventWriter writer;
  Matcher matcher(reader, writer, OrderBookConfig{100, 200, 8});

  (void)matcher.process(new_limit(1, 1, 101, Side::Ask, 105, 4));
  (void)matcher.process(new_limit(2, 2, 202, Side::Ask, 105, 6));
  const ProcessResult processed =
      matcher.process(new_limit(3, 3, 303, Side::Bid, 106, 7));

  const auto best_ask = matcher.book().best(Side::Ask);
  if (processed.status != ProcessStatus::Continue || !best_ask ||
      best_ask->id != 2 || best_ask->remaining != 3) {
    return false;
  }

  std::size_t trade_count = 0;
  bool first_trade_ok = false;
  bool second_trade_ok = false;
  for (std::size_t i = 0; i < writer.event_count; ++i) {
    const Event& event = writer.events[i].payload.message;
    if (event.type != EventType::Trade) {
      continue;
    }
    if (trade_count == 0) {
      first_trade_ok = event.trade.maker_order_id == 1 &&
                       event.trade.price == 105 &&
                       event.trade.quantity == 4;
    } else if (trade_count == 1) {
      second_trade_ok = event.trade.maker_order_id == 2 &&
                        event.trade.price == 105 &&
                        event.trade.quantity == 3;
    }
    ++trade_count;
  }

  return trade_count == 2 && first_trade_ok && second_trade_ok &&
         matcher.book().validate_invariants();
}

[[nodiscard]] bool rejects_zero_quantity() {
  ShutdownCommandReader reader;
  CollectingEventWriter writer;
  Matcher matcher(reader, writer, OrderBookConfig{100, 200, 8});

  const ProcessResult processed =
      matcher.process(new_limit(1, 1, 101, Side::Bid, 150, 0));

  return processed.status == ProcessStatus::Continue &&
         writer.event_count == 1 &&
         event_type_is(writer, 0, EventType::OrderRejected) &&
         writer.events[0].payload.message.rejected.reason ==
             RejectReason::InvalidQuantity &&
         !matcher.book().best(Side::Bid);
}

[[nodiscard]] bool default_command_is_not_shutdown() {
  ShutdownCommandReader reader;
  CollectingEventWriter writer;
  Matcher matcher(reader, writer, OrderBookConfig{100, 200, 8});

  const ProcessResult processed = matcher.process(envelope(1, Command{}));

  return processed.status == ProcessStatus::Continue &&
         writer.event_count == 1 &&
         event_type_is(writer, 0, EventType::OrderRejected) &&
         writer.events[0].payload.message.rejected.reason ==
             RejectReason::UnknownCommand;
}

[[nodiscard]] bool command_wal_payload_preserves_client_id() {
  const CommandEnvelope command =
      envelope(7, Command{SaveSnapshotCommand{42, 3}});
  return command.command_sequence == 7 &&
         command.payload.client_id == test_client_id &&
         command.payload.message.type == CommandType::SaveSnapshot;
}

[[nodiscard]] bool shutdown_is_forwarded_as_a_complete_command_result() {
  ShutdownCommandReader reader;
  CollectingEventWriter writer;
  Matcher matcher(reader, writer, OrderBookConfig{100, 200, 8}, 41);

  const RunResult result = matcher.run();
  return result.status == RunStatus::Stopped && writer.event_count == 1 &&
         event_type_is(writer, 0, EventType::Shutdown) &&
         event_metadata_is(writer, 0, 41, 1, 0, true);
}

[[nodiscard]] bool command_reader_fatal_is_not_an_event() {
  FatalCommandReader reader;
  CollectingEventWriter writer;
  Matcher matcher(reader, writer, OrderBookConfig{100, 200, 8});

  const RunResult result = matcher.run();
  return result.status == RunStatus::Fatal &&
         result.fatal_reason == FatalReason::CommandReaderFatal &&
         matcher.fatal() && writer.event_count == 0;
}

[[nodiscard]] bool fatal_publish_stops_matcher() {
  ShutdownCommandReader reader;
  CollectingEventWriter writer;
  writer.fail_after = 1;
  Matcher matcher(reader, writer, OrderBookConfig{100, 200, 8});

  const ProcessResult processed =
      matcher.process(new_limit(1, 1, 101, Side::Bid, 150, 10));
  const RunResult rerun = matcher.run();

  return processed.status == ProcessStatus::Fatal &&
         processed.fatal_reason == FatalReason::EventWriterFatal &&
         matcher.fatal() &&
         matcher.fatal_reason() == FatalReason::EventWriterFatal &&
         writer.event_count == 1 &&
         !writer.events[0].payload.is_last_for_command &&
         rerun.status == RunStatus::Fatal &&
         rerun.fatal_reason == FatalReason::EventWriterFatal;
}

[[nodiscard]] bool accepts_strictly_increasing_order_ids() {
  ShutdownCommandReader reader;
  CollectingEventWriter writer;
  Matcher matcher(reader, writer, OrderBookConfig{100, 200, 8});

  const ProcessResult first =
      matcher.process(new_limit(1, 1, 101, Side::Bid, 150, 1));
  const ProcessResult second =
      matcher.process(new_limit(2, 2, 102, Side::Bid, 151, 1));
  const ProcessResult third =
      matcher.process(new_limit(3, 3, 103, Side::Bid, 152, 1));

  return first.status == ProcessStatus::Continue &&
         second.status == ProcessStatus::Continue &&
         third.status == ProcessStatus::Continue && !matcher.fatal() &&
         matcher.book().validate_invariants();
}

[[nodiscard]] bool repeated_order_id_is_fatal() {
  ShutdownCommandReader reader;
  CollectingEventWriter writer;
  Matcher matcher(reader, writer, OrderBookConfig{100, 200, 8});

  (void)matcher.process(new_limit(1, 1, 101, Side::Bid, 150, 1));
  (void)matcher.process(new_limit(2, 2, 102, Side::Bid, 151, 1));
  const std::size_t event_count_before = writer.event_count;
  const ProcessResult repeated =
      matcher.process(new_limit(3, 2, 202, Side::Bid, 152, 1));

  return repeated.status == ProcessStatus::Fatal &&
         repeated.fatal_reason == FatalReason::NonMonotonicOrderId &&
         matcher.fatal() &&
         matcher.fatal_reason() == FatalReason::NonMonotonicOrderId &&
         writer.event_count == event_count_before + 1 &&
         event_type_is(writer, event_count_before, EventType::MatcherFatal) &&
         writer.events[event_count_before].payload.message.fatal.reason ==
             FatalReason::NonMonotonicOrderId &&
         writer.events[event_count_before]
                 .payload.message.fatal.offending_order_id == 2 &&
         writer.events[event_count_before]
                 .payload.message.fatal.last_order_id == 2;
}

[[nodiscard]] bool out_of_order_id_is_fatal() {
  ShutdownCommandReader reader;
  CollectingEventWriter writer;
  Matcher matcher(reader, writer, OrderBookConfig{100, 200, 8});

  (void)matcher.process(new_limit(1, 1, 101, Side::Bid, 150, 1));
  (void)matcher.process(new_limit(2, 3, 103, Side::Bid, 151, 1));
  const ProcessResult stale =
      matcher.process(new_limit(3, 2, 102, Side::Bid, 152, 1));

  return stale.status == ProcessStatus::Fatal &&
         stale.fatal_reason == FatalReason::NonMonotonicOrderId &&
         matcher.fatal_reason() == FatalReason::NonMonotonicOrderId &&
         event_type_is(writer, writer.event_count - 1,
                       EventType::MatcherFatal) &&
         writer.events[writer.event_count - 1]
                 .payload.message.fatal.offending_order_id == 2 &&
         writer.events[writer.event_count - 1]
                 .payload.message.fatal.last_order_id == 3;
}

[[nodiscard]] bool first_duplicate_pair_is_fatal() {
  ShutdownCommandReader reader;
  CollectingEventWriter writer;
  Matcher matcher(reader, writer, OrderBookConfig{100, 200, 8});

  const ProcessResult first =
      matcher.process(new_limit(1, 5, 105, Side::Bid, 150, 1));
  const ProcessResult repeated =
      matcher.process(new_limit(2, 5, 205, Side::Bid, 151, 1));

  return first.status == ProcessStatus::Continue &&
         repeated.status == ProcessStatus::Fatal &&
         repeated.fatal_reason == FatalReason::NonMonotonicOrderId &&
         matcher.fatal() &&
         event_type_is(writer, writer.event_count - 1,
                       EventType::MatcherFatal) &&
         writer.events[writer.event_count - 1]
                 .payload.message.fatal.offending_order_id == 5 &&
         writer.events[writer.event_count - 1]
                 .payload.message.fatal.last_order_id == 5;
}

[[nodiscard]] bool rejected_order_id_is_consumed() {
  ShutdownCommandReader reader;
  CollectingEventWriter writer;
  Matcher matcher(reader, writer, OrderBookConfig{100, 200, 8});

  const ProcessResult rejected =
      matcher.process(new_limit(1, 10, 110, Side::Bid, 150, 0));
  const ProcessResult accepted =
      matcher.process(new_limit(2, 11, 111, Side::Bid, 150, 1));

  return rejected.status == ProcessStatus::Continue &&
         accepted.status == ProcessStatus::Continue && !matcher.fatal() &&
         writer.event_count == 3 &&
         event_type_is(writer, 0, EventType::OrderRejected) &&
         event_type_is(writer, 1, EventType::OrderAccepted) &&
         event_type_is(writer, 2, EventType::OrderRested);
}

[[nodiscard]] bool rejected_order_id_cannot_repeat() {
  ShutdownCommandReader reader;
  CollectingEventWriter writer;
  Matcher matcher(reader, writer, OrderBookConfig{100, 200, 8});

  const ProcessResult rejected =
      matcher.process(new_limit(1, 10, 110, Side::Bid, 150, 0));
  const std::size_t event_count_before = writer.event_count;
  const ProcessResult repeated =
      matcher.process(new_limit(2, 10, 210, Side::Bid, 150, 0));

  return rejected.status == ProcessStatus::Continue &&
         repeated.status == ProcessStatus::Fatal &&
         repeated.fatal_reason == FatalReason::NonMonotonicOrderId &&
         writer.event_count == event_count_before + 1 &&
         event_type_is(writer, event_count_before, EventType::MatcherFatal) &&
         writer.events[event_count_before].payload.message.fatal.reason ==
             FatalReason::NonMonotonicOrderId;
}

[[nodiscard]] bool save_snapshot_captures_ordered_barrier_state() {
  ShutdownCommandReader reader;
  CollectingEventWriter writer;
  MemorySnapshotStore snapshots;
  Matcher<ShutdownCommandReader, CollectingEventWriter, MemorySnapshotStore>
      matcher(reader, writer, snapshots, OrderBookConfig{100, 200, 8});

  if (matcher.process(new_limit(1, 1, 101, Side::Bid, 150, 10)).fatal()) {
    return false;
  }
  const ProcessResult saved = matcher.process(save_snapshot(2, 42, 3));
  if (saved.status != ProcessStatus::Continue ||
      snapshots.capture_count != 1 || snapshots.orders.size() != 1 ||
      snapshots.image.last_order_id != 1 ||
      !event_type_is(writer, writer.event_count - 1,
                     EventType::SaveSnapshot) ||
      writer.events[writer.event_count - 1]
              .payload.message.save_snapshot.snapshot_id != 42 ||
      writer.events[writer.event_count - 1]
              .payload.caused_by_command_sequence != 2 ||
      writer.events[writer.event_count - 1]
              .payload.message.save_snapshot.snapshot_epoch_id != 3) {
    return false;
  }

  if (matcher.process(new_limit(3, 2, 202, Side::Bid, 151, 20)).fatal()) {
    return false;
  }

  return snapshots.orders.size() == 1 && snapshots.orders[0].id == 1 &&
         snapshots.orders[0].remaining == 10 &&
         matcher.book().best(Side::Bid)->id == 2;
}

[[nodiscard]] bool load_snapshot_restores_book_and_order_id_boundary() {
  ShutdownCommandReader reader;
  CollectingEventWriter writer;
  MemorySnapshotStore snapshots;
  Matcher<ShutdownCommandReader, CollectingEventWriter, MemorySnapshotStore>
      matcher(reader, writer, snapshots, OrderBookConfig{100, 200, 8});

  if (matcher.process(new_limit(1, 1, 101, Side::Bid, 150, 10)).fatal() ||
      matcher.process(save_snapshot(2, 42, 3)).fatal() ||
      matcher.process(new_limit(3, 2, 202, Side::Bid, 151, 20)).fatal()) {
    return false;
  }

  const ProcessResult loaded = matcher.process(load_snapshot(4, 42, 3));
  if (loaded.status != ProcessStatus::Continue || snapshots.load_count != 1 ||
      !event_type_is(writer, writer.event_count - 1,
                     EventType::LoadSnapshot) ||
      writer.events[writer.event_count - 1]
              .payload.message.load_snapshot.snapshot_id != 42 ||
      writer.events[writer.event_count - 1]
              .payload.caused_by_command_sequence != 4 ||
      writer.events[writer.event_count - 1]
              .payload.message.load_snapshot.snapshot_epoch_id != 3) {
    return false;
  }

  const auto best_bid = matcher.book().best(Side::Bid);
  if (!best_bid || best_bid->id != 1 || best_bid->remaining != 10) {
    return false;
  }

  const ProcessResult repeated =
      matcher.process(new_limit(5, 1, 301, Side::Bid, 152, 1));
  return repeated.status == ProcessStatus::Fatal &&
         repeated.fatal_reason == FatalReason::NonMonotonicOrderId;
}

[[nodiscard]] bool snapshot_barrier_publish_failure_is_terminal() {
  ShutdownCommandReader reader;
  CollectingEventWriter writer;
  MemorySnapshotStore snapshots;
  writer.fail_after = 2;
  Matcher<ShutdownCommandReader, CollectingEventWriter, MemorySnapshotStore>
      matcher(reader, writer, snapshots, OrderBookConfig{100, 200, 8});

  if (matcher.process(new_limit(1, 1, 101, Side::Bid, 150, 10)).fatal()) {
    return false;
  }

  const ProcessResult saved = matcher.process(save_snapshot(2, 42, 3));
  return saved.status == ProcessStatus::Fatal &&
         saved.fatal_reason == FatalReason::EventWriterFatal &&
         snapshots.capture_count == 1 && matcher.fatal();
}

[[nodiscard]] bool snapshot_capture_failure_is_terminal() {
  ShutdownCommandReader reader;
  CollectingEventWriter writer;
  MemorySnapshotStore snapshots;
  snapshots.fail_capture = true;
  Matcher<ShutdownCommandReader, CollectingEventWriter, MemorySnapshotStore>
      matcher(reader, writer, snapshots, OrderBookConfig{100, 200, 8});

  const ProcessResult saved = matcher.process(save_snapshot(1, 42, 3));
  return saved.status == ProcessStatus::Fatal &&
         saved.fatal_reason == FatalReason::SnapshotCaptureFailed &&
         matcher.fatal_reason() == FatalReason::SnapshotCaptureFailed;
}

} // namespace

int main() {
  if (!rests_non_crossing_limit()) {
    return EXIT_FAILURE;
  }
  if (!matches_fifo_at_resting_price()) {
    return EXIT_FAILURE;
  }
  if (!rejects_zero_quantity()) {
    return EXIT_FAILURE;
  }
  if (!default_command_is_not_shutdown()) {
    return EXIT_FAILURE;
  }
  if (!command_wal_payload_preserves_client_id()) {
    return EXIT_FAILURE;
  }
  if (!replay_commands_are_forwarded()) {
    return EXIT_FAILURE;
  }
  if (!shutdown_is_forwarded_as_a_complete_command_result()) {
    return EXIT_FAILURE;
  }
  if (!command_reader_fatal_is_not_an_event()) {
    return EXIT_FAILURE;
  }
  if (!fatal_publish_stops_matcher()) {
    return EXIT_FAILURE;
  }
  if (!accepts_strictly_increasing_order_ids()) {
    return EXIT_FAILURE;
  }
  if (!repeated_order_id_is_fatal()) {
    return EXIT_FAILURE;
  }
  if (!out_of_order_id_is_fatal()) {
    return EXIT_FAILURE;
  }
  if (!first_duplicate_pair_is_fatal()) {
    return EXIT_FAILURE;
  }
  if (!rejected_order_id_is_consumed()) {
    return EXIT_FAILURE;
  }
  if (!rejected_order_id_cannot_repeat()) {
    return EXIT_FAILURE;
  }
  if (!save_snapshot_captures_ordered_barrier_state()) {
    return EXIT_FAILURE;
  }
  if (!load_snapshot_restores_book_and_order_id_boundary()) {
    return EXIT_FAILURE;
  }
  if (!snapshot_capture_failure_is_terminal()) {
    return EXIT_FAILURE;
  }
  if (!snapshot_barrier_publish_failure_is_terminal()) {
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
