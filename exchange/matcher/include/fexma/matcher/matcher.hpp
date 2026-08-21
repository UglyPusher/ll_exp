/**
 * @file matcher.hpp
 * @brief Minimal single-writer matcher sample built on OrderBook.
 */
#pragma once

#include <algorithm>
#include <optional>
#include <type_traits>

#include <fexma/matcher/types.hpp>
#include <fexma/order_book/order_book.hpp>

namespace fexma::matcher {

class NullSnapshotStore final {
public:
  [[nodiscard]] SnapshotOperationResult
  capture(const SaveSnapshotCommand&, const MatcherSnapshotView&) noexcept {
    return {SnapshotOperationStatus::Ok};
  }

  [[nodiscard]] SnapshotOperationResult
  load(const LoadSnapshotCommand&, MatcherSnapshotImage&) noexcept {
    return {SnapshotOperationStatus::Unavailable};
  }
};

template <class CommandReader, class EventWriter,
          class SnapshotStore = NullSnapshotStore>
class Matcher final {
public:
  Matcher(CommandReader& command_reader, EventWriter& event_writer,
          const OrderBookConfig& book_config,
          EventSequence first_event_sequence = 1)
      requires std::is_same_v<SnapshotStore, NullSnapshotStore>
      : command_reader_(command_reader),
        event_writer_(event_writer),
        snapshot_store_(&default_snapshot_store_),
        book_config_(book_config),
        book_(book_config),
        next_event_sequence_(first_event_sequence) {}

  Matcher(CommandReader& command_reader, EventWriter& event_writer,
          SnapshotStore& snapshot_store, const OrderBookConfig& book_config,
          EventSequence first_event_sequence = 1)
      : command_reader_(command_reader),
        event_writer_(event_writer),
        snapshot_store_(&snapshot_store),
        book_config_(book_config),
        book_(book_config),
        next_event_sequence_(first_event_sequence) {}

  Matcher(const Matcher&) = delete;
  Matcher& operator=(const Matcher&) = delete;
  Matcher(Matcher&&) = delete;
  Matcher& operator=(Matcher&&) = delete;

  [[nodiscard]] RunResult run() noexcept {
    if (fatal_) {
      return {RunStatus::Fatal, fatal_reason_};
    }

    while (running_) {
      const CommandReadResult read = command_reader_.read_next();
      switch (read.status) {
      case CommandReadStatus::Ok: {
        const ProcessResult processed = process(read.envelope);
        if (processed.status == ProcessStatus::Stop) {
          return {RunStatus::Stopped};
        }
        if (processed.status == ProcessStatus::Fatal) {
          return {RunStatus::Fatal, processed.fatal_reason};
        }
        break;
      }
      case CommandReadStatus::Empty:
        continue;
      case CommandReadStatus::Fatal:
        transition_to_fatal_state(FatalReason::CommandReaderFatal);
        return {RunStatus::Fatal, fatal_reason_};
      }
    }

    return {RunStatus::Stopped};
  }

  [[nodiscard]] ProcessResult
  process(const CommandEnvelope& envelope) noexcept {
    if (fatal_) {
      return {ProcessStatus::Fatal, fatal_reason_};
    }

    CommandEventBatch events(*this, envelope.payload.client_id,
                             envelope.command_sequence);
    const Command& command = envelope.payload.message;
    ProcessResult processed{};
    switch (command.type) {
    case CommandType::None:
      if (!events.add(
              OrderRejectedEvent{{}, RejectReason::UnknownCommand})) {
        return {ProcessStatus::Fatal, fatal_reason_};
      }
      break;
    case CommandType::NewLimit:
      processed = process_new_limit(command.new_limit, events);
      break;
    case CommandType::SaveSnapshot:
      processed = process_save_snapshot(
          command.save_snapshot, envelope.command_sequence, events);
      break;
    case CommandType::LoadSnapshot:
      processed = process_load_snapshot(command.load_snapshot, events);
      break;
    case CommandType::Shutdown:
      if (!events.add(ShutdownEvent{})) {
        return {ProcessStatus::Fatal, fatal_reason_};
      }
      running_ = false;
      processed = {ProcessStatus::Stop};
      break;
    case CommandType::StartReplay:
      if (!events.add(StartReplayEvent{
              command.start_replay.replay_id,
              command.start_replay.live_snapshot_id,
              command.start_replay.replay_snapshot_id,
              command.start_replay.replay_through_command_sequence})) {
        return {ProcessStatus::Fatal, fatal_reason_};
      }
      if (replay_mode_ == ReplayMode::Live) {
        replay_id_ = command.start_replay.replay_id;
        live_snapshot_id_ = command.start_replay.live_snapshot_id;
        replay_snapshot_id_ = command.start_replay.replay_snapshot_id;
        deferred_start_replay_ = true;
      }
      break;
    case CommandType::StopReplay:
      if (!events.add(
              StopReplayEvent{command.stop_replay.replay_id})) {
        return {ProcessStatus::Fatal, fatal_reason_};
      }
      if (replay_mode_ == ReplayMode::Replay &&
          command.stop_replay.replay_id == replay_id_) {
        deferred_mode_ = ReplayMode::Restoring;
      }
      break;
    default:
      if (!events.add(
              OrderRejectedEvent{{}, RejectReason::UnknownCommand})) {
        return {ProcessStatus::Fatal, fatal_reason_};
      }
      break;
    }

    if (!events.failed() && events.empty()) {
      if (!events.add(MatcherFatalEvent{
              FatalReason::CommandProducedNoEvent, {}, last_order_id_})) {
        return {ProcessStatus::Fatal, fatal_reason_};
      }
      transition_to_fatal_state(FatalReason::CommandProducedNoEvent);
      processed = {ProcessStatus::Fatal,
                   FatalReason::CommandProducedNoEvent};
    }

    if (!events.failed() && !events.finish()) {
      return {ProcessStatus::Fatal, fatal_reason_};
    }
    apply_deferred_replay_transition();
    return processed;
  }

  [[nodiscard]] bool fatal() const noexcept {
    return fatal_;
  }

  [[nodiscard]] FatalReason fatal_reason() const noexcept {
    return fatal_reason_;
  }

  [[nodiscard]] const order_book::OrderBook& book() const noexcept {
    return book_;
  }

  [[nodiscard]] OrderId last_order_id() const noexcept {
    return last_order_id_;
  }

  [[nodiscard]] EventSequence next_event_sequence() const noexcept {
    return next_event_sequence_;
  }

  [[nodiscard]] ReplayMode replay_mode() const noexcept {
    return replay_mode_;
  }

private:
  class CommandEventBatch final {
  public:
    CommandEventBatch(Matcher& matcher, ClientId client_id,
                      CommandSequence command_sequence) noexcept
        : matcher_(matcher),
          client_id_(client_id),
          command_sequence_(command_sequence) {}

    template <class Payload>
    [[nodiscard]] bool add(const Payload& payload) noexcept {
      if (has_pending_ && !publish_pending(false)) {
        return false;
      }

      pending_ = Event{payload};
      pending_index_ = next_index_++;
      has_pending_ = true;
      return true;
    }

    [[nodiscard]] bool finish() noexcept {
      return has_pending_ && publish_pending(true);
    }

    [[nodiscard]] bool failed() const noexcept {
      return failed_;
    }

    [[nodiscard]] bool empty() const noexcept {
      return !has_pending_;
    }

  private:
    [[nodiscard]] bool publish_pending(bool is_last_for_command) noexcept {
      const EventEnvelope envelope{
          matcher_.next_event_sequence_,
          {client_id_, command_sequence_, pending_index_,
           is_last_for_command, pending_}};
      if (!matcher_.publish_event(envelope)) {
        failed_ = true;
        return false;
      }
      has_pending_ = false;
      return true;
    }

    Matcher& matcher_;
    ClientId client_id_{};
    CommandSequence command_sequence_{};
    Event pending_{};
    EventIndex pending_index_{};
    EventIndex next_index_{};
    bool has_pending_{};
    bool failed_{};
  };

  [[nodiscard]] ProcessResult process_save_snapshot(
      const SaveSnapshotCommand& command,
      CommandSequence command_sequence, CommandEventBatch& events) noexcept {
    const MatcherSnapshotView snapshot{command.snapshot_id,
                                       command_sequence,
                                       command.snapshot_epoch_id,
                                       last_order_id_,
                                       next_event_sequence_ + 1,
                                       book_config_,
                                       &book_};
    const SnapshotOperationResult captured =
        snapshot_store_->capture(command, snapshot);
    if (!captured.ok()) {
      return enter_fatal_process(FatalReason::SnapshotCaptureFailed, {},
                                 last_order_id_, events);
    }
    if (!events.add(SaveSnapshotEvent{command.snapshot_id,
                                      command.snapshot_epoch_id})) {
      return {ProcessStatus::Fatal, fatal_reason_};
    }
    return {ProcessStatus::Continue};
  }

  [[nodiscard]] ProcessResult process_load_snapshot(
      const LoadSnapshotCommand& command, CommandEventBatch& events) noexcept {
    MatcherSnapshotImage snapshot{};
    const SnapshotOperationResult loaded =
        snapshot_store_->load(command, snapshot);
    if (!loaded.ok()) {
      const FatalReason reason =
          loaded.status == SnapshotOperationStatus::Unavailable
              ? FatalReason::SnapshotLoadUnavailable
              : FatalReason::SnapshotLoadInvalid;
      return enter_fatal_process(reason, {}, last_order_id_, events);
    }

    if (!same_config(snapshot.book_config, book_config_) ||
        snapshot.snapshot_id != command.snapshot_id ||
        snapshot.epoch_id != command.snapshot_epoch_id) {
      return enter_fatal_process(FatalReason::SnapshotLoadInvalid, {},
                                 last_order_id_, events);
    }

    const order_book::RestoreResult restored = book_.restore(snapshot.orders);
    if (!restored.ok()) {
      return enter_fatal_process(FatalReason::SnapshotLoadInvalid, {},
                                 last_order_id_, events);
    }

    last_order_id_ = snapshot.last_order_id;
    if (replay_mode_ == ReplayMode::Replay &&
        command.snapshot_id == replay_snapshot_id_) {
      deferred_event_sequence_ = snapshot.next_event_sequence;
    } else if (replay_mode_ == ReplayMode::Restoring &&
               command.snapshot_id == live_snapshot_id_) {
      deferred_event_sequence_ = live_resume_event_sequence_;
      deferred_mode_ = ReplayMode::Live;
    }
    if (!events.add(LoadSnapshotEvent{command.snapshot_id,
                                      command.snapshot_epoch_id})) {
      return {ProcessStatus::Fatal, fatal_reason_};
    }
    return {ProcessStatus::Continue};
  }

  [[nodiscard]] ProcessResult process_new_limit(
      const NewLimitOrder& incoming, CommandEventBatch& events) noexcept {
    if (incoming.id <= last_order_id_) {
      return enter_fatal_process(FatalReason::NonMonotonicOrderId,
                                 incoming.id, last_order_id_, events);
    }
    last_order_id_ = incoming.id;

    if (incoming.quantity == 0) {
      if (!events.add(OrderRejectedEvent{incoming.id,
                                         RejectReason::InvalidQuantity})) {
        return {ProcessStatus::Fatal, fatal_reason_};
      }
      return {ProcessStatus::Continue};
    }

    if (!events.add(OrderAcceptedEvent{incoming.id})) {
      return {ProcessStatus::Fatal, fatal_reason_};
    }

    Quantity remaining = incoming.quantity;
    const Side opposite = opposite_side(incoming.side);

    while (remaining != 0) {
      const std::optional<order_book::OrderView> resting =
          book_.best(opposite);
      if (!resting || !crosses(incoming.side, incoming.price, resting->price)) {
        break;
      }

      const Quantity executed =
          (std::min)(remaining, resting->remaining);
      if (!events.add(TradeEvent{incoming.id, resting->id, incoming.owner_id,
                                 resting->owner_id, resting->price,
                                 executed})) {
        return {ProcessStatus::Fatal, fatal_reason_};
      }

      remaining -= executed;
      const Quantity resting_remaining = resting->remaining - executed;
      if (resting_remaining == 0) {
        const order_book::EraseResult erased = book_.erase(resting->id);
        if (!erased.ok()) {
          return enter_fatal_process(
              FatalReason::BookEraseFailedAfterExecution, incoming.id,
              last_order_id_, events);
        }
        if (!events.add(OrderDoneEvent{resting->id})) {
          return {ProcessStatus::Fatal, fatal_reason_};
        }
      } else {
        const order_book::SetRemainingResult changed =
            book_.set_remaining(resting->id, resting_remaining);
        if (!changed.ok()) {
          return enter_fatal_process(
              FatalReason::BookUpdateFailedAfterExecution, incoming.id,
              last_order_id_, events);
        }
      }
    }

    if (remaining == 0) {
      if (!events.add(OrderDoneEvent{incoming.id})) {
        return {ProcessStatus::Fatal, fatal_reason_};
      }
      return {ProcessStatus::Continue};
    }

    const order_book::InsertResult inserted =
        book_.insert({incoming.id, incoming.owner_id, incoming.side,
                      incoming.price, remaining});
    if (!inserted.ok()) {
      if (remaining == incoming.quantity) {
        if (!events.add(OrderRejectedEvent{
                incoming.id, RejectReason::BookInsertFailed})) {
          return {ProcessStatus::Fatal, fatal_reason_};
        }
        return {ProcessStatus::Continue};
      }
      return enter_fatal_process(FatalReason::BookInsertFailedAfterExecution,
                                 incoming.id, last_order_id_, events);
    }

    if (!events.add(OrderRestedEvent{incoming.id, incoming.owner_id,
                                     incoming.side, incoming.price,
                                     remaining})) {
      return {ProcessStatus::Fatal, fatal_reason_};
    }
    return {ProcessStatus::Continue};
  }

  [[nodiscard]] static Side opposite_side(Side side) noexcept {
    return side == Side::Bid ? Side::Ask : Side::Bid;
  }

  [[nodiscard]] static bool crosses(Side incoming_side,
                                    PriceTick incoming_price,
                                    PriceTick resting_price) noexcept {
    return incoming_side == Side::Bid ? incoming_price >= resting_price
                                      : incoming_price <= resting_price;
  }

  [[nodiscard]] static bool same_config(
      const OrderBookConfig& lhs, const OrderBookConfig& rhs) noexcept {
    return lhs.min_price_tick == rhs.min_price_tick &&
           lhs.max_price_tick == rhs.max_price_tick &&
           lhs.max_orders == rhs.max_orders;
  }

  [[nodiscard]] bool publish_event(const EventEnvelope& envelope) noexcept {
    const PublishResult published = event_writer_.publish(envelope);
    if (!published.ok()) {
      transition_to_fatal_state(FatalReason::EventWriterFatal);
      return false;
    }
    ++next_event_sequence_;
    return true;
  }

  [[nodiscard]] ProcessResult enter_fatal_process(
      FatalReason reason, OrderId offending_order_id,
      OrderId last_order_id, CommandEventBatch& events) noexcept {
    if (reason != FatalReason::EventWriterFatal &&
        !events.add(MatcherFatalEvent{reason, offending_order_id,
                                      last_order_id})) {
      return {ProcessStatus::Fatal, fatal_reason_};
    }
    transition_to_fatal_state(reason);
    return {ProcessStatus::Fatal, fatal_reason_};
  }

  void transition_to_fatal_state(FatalReason reason) noexcept {
    fatal_ = true;
    running_ = false;
    fatal_reason_ = reason;
  }

  void apply_deferred_replay_transition() noexcept {
    if (deferred_start_replay_) {
      live_resume_event_sequence_ = next_event_sequence_;
      replay_mode_ = ReplayMode::Replay;
      deferred_start_replay_ = false;
    }
    if (deferred_event_sequence_ != 0) {
      next_event_sequence_ = deferred_event_sequence_;
      deferred_event_sequence_ = 0;
    }
    if (deferred_mode_.has_value()) {
      replay_mode_ = *deferred_mode_;
      deferred_mode_.reset();
    }
  }

  CommandReader& command_reader_;
  EventWriter& event_writer_;
  SnapshotStore* snapshot_store_{};
  OrderBookConfig book_config_{};
  order_book::OrderBook book_;
  OrderId last_order_id_{};
  EventSequence next_event_sequence_{1};
  EventSequence live_resume_event_sequence_{};
  EventSequence deferred_event_sequence_{};
  ReplayId replay_id_{};
  SnapshotId live_snapshot_id_{};
  SnapshotId replay_snapshot_id_{};
  ReplayMode replay_mode_{ReplayMode::Live};
  std::optional<ReplayMode> deferred_mode_{};
  bool deferred_start_replay_{};
  bool running_{true};
  bool fatal_{false};
  FatalReason fatal_reason_{FatalReason::None};

  inline static NullSnapshotStore default_snapshot_store_{};
};

} // namespace fexma::matcher
