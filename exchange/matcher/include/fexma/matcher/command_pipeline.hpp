/**
 * @file command_pipeline.hpp
 * @brief Bounded preallocated Command SPMC stage pipeline.
 */
#pragma once

#include <fexma/matcher/types.hpp>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace fexma::matcher {

inline constexpr std::size_t command_pipeline_cache_line_size = 64;

struct CommandPipelineConfig {
  std::uint32_t capacity{};
  CommandSequence first_sequence{1};
};

enum class CommandPipelineStatus : std::uint8_t {
  Ok,
  Empty,
  Full,
  InvalidConfig,
  AllocationFailed,
  InvalidCount,
  InvalidDecision,
  SequenceExhausted,
  PersistenceFailed,
  Closed,
  AlreadyOpen
};

struct CommandPipelineResult {
  CommandPipelineStatus status{CommandPipelineStatus::Closed};
  CommandSequence sequence{};
  std::uint64_t frontier{};

  [[nodiscard]] bool ok() const noexcept {
    return status == CommandPipelineStatus::Ok;
  }
};

struct CommandPipelineSnapshot {
  std::uint64_t tail{};
  std::uint64_t reserve_checked{};
  std::uint64_t risk_checked{};
  std::uint64_t durable{};
  std::uint64_t head{};
  bool persistence_failed{};
};

class CommandPipeline final {
public:
  CommandPipeline() = default;
  ~CommandPipeline() = default;

  CommandPipeline(const CommandPipeline&) = delete;
  CommandPipeline& operator=(const CommandPipeline&) = delete;
  CommandPipeline(CommandPipeline&&) = delete;
  CommandPipeline& operator=(CommandPipeline&&) = delete;

  [[nodiscard]] CommandPipelineStatus
  open(const CommandPipelineConfig& config) noexcept;

  [[nodiscard]] CommandPipelineResult
  try_publish(const CommandWalPayload& payload) noexcept;

  [[nodiscard]] CommandPipelineResult
  copy_pending_for_persistence(std::uint32_t batch_offset,
                               CommandEnvelope& command) const noexcept;

  [[nodiscard]] CommandPipelineResult
  publish_durable(std::uint32_t count) noexcept;

  void fail_persistence() noexcept;

  [[nodiscard]] CommandPipelineResult
  try_read_for_risk(CommandEnvelope& command) const noexcept;

  [[nodiscard]] CommandPipelineResult
  publish_risk(const RiskResult& result) noexcept;

  [[nodiscard]] CommandPipelineResult
  try_read_for_reserve(CommandEnvelope& command,
                       RiskResult& risk) const noexcept;

  [[nodiscard]] CommandPipelineResult
  publish_reserve(const ReserveResult& result) noexcept;

  [[nodiscard]] CommandPipelineResult
  try_consume(CommandRingSlot& slot) noexcept;

  [[nodiscard]] CommandPipelineSnapshot snapshot() const noexcept;
  [[nodiscard]] bool is_open() const noexcept;
  [[nodiscard]] const CommandPipelineConfig& config() const noexcept;

private:
  struct alignas(command_pipeline_cache_line_size) Frontier {
    std::atomic<std::uint64_t> value{};
    std::array<std::byte,
               command_pipeline_cache_line_size -
                   sizeof(std::atomic<std::uint64_t>)>
        padding{};
  };

  struct alignas(command_pipeline_cache_line_size) FailureState {
    std::atomic<bool> persistence_failed{};
    std::array<std::byte,
               command_pipeline_cache_line_size - sizeof(std::atomic<bool>)>
        padding{};
  };

  struct alignas(command_pipeline_cache_line_size) ControlBlock {
    Frontier tail{};
    Frontier reserve_checked{};
    Frontier risk_checked{};
    Frontier durable{};
    Frontier head{};
    FailureState failure{};
  };

  [[nodiscard]] CommandRingSlot& slot_at(std::uint64_t position) noexcept;
  [[nodiscard]] const CommandRingSlot&
  slot_at(std::uint64_t position) const noexcept;
  [[nodiscard]] CommandSequence sequence_at(std::uint64_t position) const
      noexcept;

  static_assert(sizeof(Frontier) == command_pipeline_cache_line_size);
  static_assert(sizeof(FailureState) == command_pipeline_cache_line_size);
  static_assert(alignof(Frontier) == command_pipeline_cache_line_size);
  static_assert(alignof(FailureState) == command_pipeline_cache_line_size);
  static_assert(sizeof(ControlBlock) == 6 * command_pipeline_cache_line_size);
  static_assert(alignof(ControlBlock) == command_pipeline_cache_line_size);

  std::unique_ptr<CommandRingSlot[]> slots_{};
  std::unique_ptr<ControlBlock> control_{};
  CommandPipelineConfig config_{};
  bool sequence_exhausted_{};
  bool open_{};
};

} // namespace fexma::matcher
