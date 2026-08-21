/**
 * @file replay.cpp
 * @brief Validated WAL replay source and Event WAL comparison.
 */
#include <fexma/matcher/replay.hpp>

#include <algorithm>
#include <limits>

namespace fexma::matcher {

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
