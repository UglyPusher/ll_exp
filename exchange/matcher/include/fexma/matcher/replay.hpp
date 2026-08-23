/**
 * @file replay.hpp
 * @brief Validated WAL replay source and canonical Event WAL comparator.
 */
#pragma once

#include <fexma/matcher/codec.hpp>
#include <fexma/matcher/command_pipeline.hpp>
#include <fexma/wal/reader.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <span>

namespace fexma::matcher {

enum class ReplayStatus : std::uint8_t {
  Ok,
  Complete,
  Backpressure,
  InvalidConfig,
  WalOpenFailed,
  WalReadFailed,
  DecodeFailed,
  PipelineFailed,
  ExpectedEventMissing,
  UnexpectedEvent,
  EventMismatch,
  InvalidTransition,
  ManifestMismatch,
  CheckpointCapacityExceeded
};

struct ReplayResult {
  ReplayStatus status{ReplayStatus::InvalidConfig};
  std::uint64_t sequence{};

  [[nodiscard]] bool ok() const noexcept {
    return status == ReplayStatus::Ok;
  }
};

enum class PersistenceAction : std::uint8_t {
  AppendAndSync,
  AdvanceDurableOnly
};

class ReplayPersistenceState final {
public:
  [[nodiscard]] PersistenceAction action() const noexcept;
  [[nodiscard]] ReplayStatus
  on_durable(const CommandEnvelope& command,
             CommandPipeline& pipeline) noexcept;
  [[nodiscard]] ReplayMode mode() const noexcept;
  [[nodiscard]] CommandSequence live_resume_sequence() const noexcept;

private:
  ReplayMode mode_{ReplayMode::Live};
  CommandSequence live_resume_sequence_{};
};

struct ReplayManifest {
  wal::ManifestId manifest_id{};
  wal::EpochId epoch_id{};
  wal::StreamId command_stream_id{};
  wal::StreamId event_stream_id{};
  CommandSchemaVersion command_schema_version{};
  EventSchemaVersion event_schema_version{};
  std::uint64_t configuration_hash{};
};

[[nodiscard]] std::uint64_t
hash_order_book_config(const OrderBookConfig& config) noexcept;

[[nodiscard]] ReplayStatus validate_replay_manifest(
    const ReplayManifest& manifest, const wal::WalConfig& command,
    const wal::WalConfig& event,
    const OrderBookConfig& book_config) noexcept;

struct MatcherStateCheckpoint {
  OrderId last_order_id{};
  EventSequence next_event_sequence{};
  std::uint32_t order_count{};
  std::uint64_t canonical_hash{};
};

[[nodiscard]] ReplayResult make_matcher_state_checkpoint(
    const order_book::OrderBook& book, const OrderBookConfig& book_config,
    OrderId last_order_id, EventSequence next_event_sequence,
    std::span<order_book::OrderView> scratch,
    MatcherStateCheckpoint& checkpoint) noexcept;

class WalFileReplaySource final {
public:
  [[nodiscard]] ReplayStatus
  open(const std::filesystem::path& path, const wal::WalConfig& expected,
       CommandSequence first_sequence,
       CommandSequence last_sequence) noexcept;

  [[nodiscard]] ReplayResult
  publish_next(CommandPipeline& pipeline) noexcept;

private:
  wal::WalReader reader_{};
  std::array<std::byte, command_wal_payload_size_v2> bytes_{};
  CommandEnvelope pending_{};
  CommandSequence first_sequence_{};
  CommandSequence last_sequence_{};
  bool has_pending_{};
  bool complete_{};
};

class EventWalComparator final {
public:
  [[nodiscard]] ReplayStatus
  open(const std::filesystem::path& path, const wal::WalConfig& expected,
       EventSequence first_sequence, EventSequence last_sequence) noexcept;

  [[nodiscard]] PublishResult publish(const EventEnvelope& event) noexcept;
  [[nodiscard]] ReplayStatus finish() noexcept;
  [[nodiscard]] ReplayResult result() const noexcept;

private:
  wal::WalReader reader_{};
  std::array<std::byte, event_wal_payload_size_v2> expected_bytes_{};
  std::array<std::byte, event_wal_payload_size_v2> actual_bytes_{};
  EventSequence first_sequence_{};
  EventSequence last_sequence_{};
  EventSequence next_sequence_{};
  ReplayResult result_{ReplayStatus::InvalidConfig, 0};
  bool open_{};
};

} // namespace fexma::matcher
