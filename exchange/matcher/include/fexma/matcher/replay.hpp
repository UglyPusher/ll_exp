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
  EventMismatch
};

struct ReplayResult {
  ReplayStatus status{ReplayStatus::InvalidConfig};
  std::uint64_t sequence{};

  [[nodiscard]] bool ok() const noexcept {
    return status == ReplayStatus::Ok;
  }
};

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
