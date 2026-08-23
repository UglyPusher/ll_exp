/**
 * @file replay.cpp
 * @brief Validated WAL replay source and Event WAL comparison.
 */
#include <fexma/matcher/replay.hpp>

#include <algorithm>
#include <limits>

namespace fexma::matcher {
namespace {

inline constexpr std::uint64_t fnv_offset = 14695981039346656037ull;
inline constexpr std::uint64_t fnv_prime = 1099511628211ull;

void hash_u8(std::uint64_t& hash, std::uint8_t value) noexcept {
  hash = (hash ^ value) * fnv_prime;
}

void hash_u32(std::uint64_t& hash, std::uint32_t value) noexcept {
  for (std::uint32_t index = 0; index < 4; ++index) {
    hash_u8(hash, static_cast<std::uint8_t>(value >> (index * 8)));
  }
}

void hash_u64(std::uint64_t& hash, std::uint64_t value) noexcept {
  for (std::uint32_t index = 0; index < 8; ++index) {
    hash_u8(hash, static_cast<std::uint8_t>(value >> (index * 8)));
  }
}

} // namespace

PersistenceAction ReplayPersistenceState::action() const noexcept {
  return mode_ == ReplayMode::Live ? PersistenceAction::AppendAndSync
                                   : PersistenceAction::AdvanceDurableOnly;
}

ReplayStatus ReplayPersistenceState::on_durable(
    const CommandEnvelope& command, CommandPipeline& pipeline) noexcept {
  const Command& message = command.payload.message;
  if (mode_ == ReplayMode::Live && message.type == CommandType::StartReplay) {
    if (command.command_sequence ==
            (std::numeric_limits<CommandSequence>::max)()) {
      return ReplayStatus::InvalidTransition;
    }
    live_resume_sequence_ = command.command_sequence + 1;
    mode_ = ReplayMode::Replay;
    return ReplayStatus::Ok;
  }

  if (mode_ == ReplayMode::Replay && message.type == CommandType::StopReplay) {
    mode_ = ReplayMode::Restoring;
    return ReplayStatus::Ok;
  }

  if (mode_ == ReplayMode::Restoring) {
    if (message.type != CommandType::LoadSnapshot) {
      return ReplayStatus::InvalidTransition;
    }
    if (pipeline.restore_live_sequence(live_resume_sequence_) !=
        CommandPipelineStatus::Ok) {
      return ReplayStatus::PipelineFailed;
    }
    mode_ = ReplayMode::Live;
  }
  return ReplayStatus::Ok;
}

ReplayMode ReplayPersistenceState::mode() const noexcept {
  return mode_;
}

CommandSequence ReplayPersistenceState::live_resume_sequence() const noexcept {
  return live_resume_sequence_;
}

std::uint64_t hash_order_book_config(const OrderBookConfig& config) noexcept {
  std::uint64_t hash = fnv_offset;
  hash_u32(hash, config.min_price_tick);
  hash_u32(hash, config.max_price_tick);
  hash_u32(hash, config.max_orders);
  return hash;
}

ReplayStatus validate_replay_manifest(
    const ReplayManifest& manifest, const wal::WalConfig& command,
    const wal::WalConfig& event,
    const OrderBookConfig& book_config) noexcept {
  if (manifest.manifest_id == 0 ||
      manifest.manifest_id != command.manifest_id ||
      manifest.manifest_id != event.manifest_id ||
      manifest.epoch_id != command.epoch_id ||
      manifest.epoch_id != event.epoch_id ||
      manifest.command_stream_id != command.stream_id ||
      manifest.event_stream_id != event.stream_id ||
      manifest.command_schema_version != command.payload_schema_version ||
      manifest.event_schema_version != event.payload_schema_version ||
      manifest.configuration_hash != hash_order_book_config(book_config)) {
    return ReplayStatus::ManifestMismatch;
  }
  return ReplayStatus::Ok;
}

ReplayResult make_matcher_state_checkpoint(
    const order_book::OrderBook& book, const OrderBookConfig& book_config,
    OrderId last_order_id, EventSequence next_event_sequence,
    std::span<order_book::OrderView> scratch,
    MatcherStateCheckpoint& checkpoint) noexcept {
  if (scratch.size() < book.order_count()) {
    return {ReplayStatus::CheckpointCapacityExceeded, 0};
  }
  const order_book::SnapshotResult copied = book.snapshot_into(scratch);
  if (!copied.ok()) {
    return {ReplayStatus::CheckpointCapacityExceeded, 0};
  }

  std::uint64_t hash = fnv_offset;
  hash_u64(hash, last_order_id);
  hash_u64(hash, next_event_sequence);
  hash_u64(hash, hash_order_book_config(book_config));
  hash_u32(hash, copied.copied);
  for (std::uint32_t index = 0; index < copied.copied; ++index) {
    const order_book::OrderView& order = scratch[index];
    hash_u64(hash, order.id);
    hash_u64(hash, order.owner_id);
    hash_u8(hash, static_cast<std::uint8_t>(order.side));
    hash_u32(hash, order.price);
    hash_u32(hash, order.remaining);
  }
  checkpoint = {last_order_id, next_event_sequence, copied.copied, hash};
  return {ReplayStatus::Ok, copied.copied};
}

ReplayStatus WalFileReplaySource::open(
    const std::filesystem::path& path, const wal::WalConfig& expected,
    CommandSequence first_sequence, CommandSequence last_sequence) noexcept {
  if (first_sequence == 0 || last_sequence < first_sequence ||
      first_sequence < expected.first_sequence ||
      expected.payload_schema_version != current_command_schema_version ||
      expected.payload_size != command_wal_payload_size_v2 ||
      expected.stream_kind != wal::StreamKind::Command) {
    return ReplayStatus::InvalidConfig;
  }
  if (!reader_.open(path, expected).ok()) {
    return ReplayStatus::WalOpenFailed;
  }

  first_sequence_ = first_sequence;
  last_sequence_ = last_sequence;
  while (reader_.next_sequence() < first_sequence_) {
    if (!reader_.read_next(bytes_).ok()) {
      return ReplayStatus::WalReadFailed;
    }
  }
  return ReplayStatus::Ok;
}

ReplayResult
WalFileReplaySource::publish_next(CommandPipeline& pipeline) noexcept {
  if (complete_) {
    return {ReplayStatus::Complete, last_sequence_};
  }

  if (!has_pending_) {
    const wal::ReadResult read = reader_.read_next(bytes_);
    if (!read.ok()) {
      return {ReplayStatus::WalReadFailed, reader_.next_sequence()};
    }
    if (read.sequence > last_sequence_) {
      complete_ = true;
      return {ReplayStatus::Complete, last_sequence_};
    }

    CommandWalPayload payload{};
    if (decode_command_wal_payload_v2(bytes_, payload) !=
        PayloadCodecStatus::Ok) {
      return {ReplayStatus::DecodeFailed, read.sequence};
    }
    pending_ = {read.sequence, payload};
    has_pending_ = true;
  }

  const CommandPipelineResult published = pipeline.try_replay(pending_);
  if (published.status == CommandPipelineStatus::Full) {
    return {ReplayStatus::Backpressure, pending_.command_sequence};
  }
  if (!published.ok()) {
    return {ReplayStatus::PipelineFailed, pending_.command_sequence};
  }

  const CommandSequence sequence = pending_.command_sequence;
  has_pending_ = false;
  if (sequence == last_sequence_) {
    complete_ = true;
  }
  return {ReplayStatus::Ok, sequence};
}

ReplayStatus EventWalComparator::open(
    const std::filesystem::path& path, const wal::WalConfig& expected,
    EventSequence first_sequence, EventSequence last_sequence) noexcept {
  if (first_sequence == 0 || last_sequence < first_sequence ||
      last_sequence == (std::numeric_limits<EventSequence>::max)() ||
      first_sequence < expected.first_sequence ||
      expected.payload_schema_version != current_event_schema_version ||
      expected.payload_size != event_wal_payload_size_v2 ||
      expected.stream_kind != wal::StreamKind::Event) {
    return ReplayStatus::InvalidConfig;
  }
  if (!reader_.open(path, expected).ok()) {
    return ReplayStatus::WalOpenFailed;
  }

  first_sequence_ = first_sequence;
  last_sequence_ = last_sequence;
  while (reader_.next_sequence() < first_sequence_) {
    if (!reader_.read_next(expected_bytes_).ok()) {
      return ReplayStatus::WalReadFailed;
    }
  }
  next_sequence_ = first_sequence_;
  result_ = {ReplayStatus::Ok, 0};
  open_ = true;
  return ReplayStatus::Ok;
}

PublishResult EventWalComparator::publish(
    const EventEnvelope& event) noexcept {
  if (!open_ || result_.status != ReplayStatus::Ok ||
      next_sequence_ > last_sequence_) {
    result_ = {ReplayStatus::UnexpectedEvent, event.event_sequence};
    return {PublishStatus::Fatal};
  }

  const wal::ReadResult read = reader_.read_next(expected_bytes_);
  if (!read.ok()) {
    result_ = {ReplayStatus::ExpectedEventMissing, event.event_sequence};
    return {PublishStatus::Fatal};
  }
  EventWalPayload expected_payload{};
  if (decode_event_wal_payload_v2(expected_bytes_, expected_payload) !=
          PayloadCodecStatus::Ok ||
      encode_event_wal_payload_v2(event.payload, actual_bytes_) !=
          PayloadCodecStatus::Ok ||
      read.sequence != event.event_sequence ||
      !std::equal(actual_bytes_.begin(), actual_bytes_.end(),
                  expected_bytes_.begin())) {
    result_ = {ReplayStatus::EventMismatch, event.event_sequence};
    return {PublishStatus::Fatal};
  }

  result_.sequence = event.event_sequence;
  ++next_sequence_;
  return {PublishStatus::Ok};
}

ReplayStatus EventWalComparator::finish() noexcept {
  if (!open_ || result_.status != ReplayStatus::Ok) {
    return result_.status;
  }
  if (next_sequence_ != last_sequence_ + 1) {
    result_ = {ReplayStatus::ExpectedEventMissing, next_sequence_};
  } else {
    result_.status = ReplayStatus::Complete;
  }
  return result_.status;
}

ReplayResult EventWalComparator::result() const noexcept {
  return result_;
}

} // namespace fexma::matcher
