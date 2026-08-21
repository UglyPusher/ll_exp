/**
 * @file command_pipeline.cpp
 * @brief Command SPMC stage pipeline implementation.
 */
#include <fexma/matcher/command_pipeline.hpp>

#include <limits>
#include <new>
#include <utility>

namespace fexma::matcher {

CommandPipelineStatus
CommandPipeline::open(const CommandPipelineConfig& config) noexcept {
  if (open_) {
    return CommandPipelineStatus::AlreadyOpen;
  }
  if (config.capacity == 0 || config.first_sequence == 0) {
    return CommandPipelineStatus::InvalidConfig;
  }

  try {
    auto slots = std::make_unique<CommandRingSlot[]>(config.capacity);
    auto control = std::make_unique<ControlBlock>();
    slots_ = std::move(slots);
    control_ = std::move(control);
  } catch (const std::bad_alloc&) {
    return CommandPipelineStatus::AllocationFailed;
  }

  config_ = config;
  control_->tail.value.store(0, std::memory_order_relaxed);
  control_->reserve_checked.value.store(0, std::memory_order_relaxed);
  control_->risk_checked.value.store(0, std::memory_order_relaxed);
  control_->durable.value.store(0, std::memory_order_relaxed);
  control_->head.value.store(0, std::memory_order_relaxed);
  control_->failure.persistence_failed.store(false,
                                             std::memory_order_relaxed);
  next_live_sequence_ = config.first_sequence;
  sequence_exhausted_ = false;
  open_ = true;
  return CommandPipelineStatus::Ok;
}

CommandPipelineResult
CommandPipeline::try_publish(const CommandWalPayload& payload) noexcept {
  if (!open_) {
    return {CommandPipelineStatus::Closed};
  }
  if (control_->failure.persistence_failed.load(std::memory_order_acquire)) {
    return {CommandPipelineStatus::PersistenceFailed};
  }
  if (sequence_exhausted_) {
    return {CommandPipelineStatus::SequenceExhausted};
  }

  const CommandSequence sequence = next_live_sequence_;
  const CommandPipelineResult published =
      try_publish_envelope({sequence, payload});
  if (!published.ok()) {
    return published;
  }

  if (sequence == (std::numeric_limits<CommandSequence>::max)()) {
    sequence_exhausted_ = true;
  } else {
    next_live_sequence_ = sequence + 1;
  }
  return published;
}

CommandPipelineResult
CommandPipeline::try_replay(const CommandEnvelope& command) noexcept {
  if (!open_) {
    return {CommandPipelineStatus::Closed};
  }
  if (control_->failure.persistence_failed.load(std::memory_order_acquire)) {
    return {CommandPipelineStatus::PersistenceFailed};
  }
  if (command.command_sequence == 0) {
    return {CommandPipelineStatus::InvalidSequence};
  }
  return try_publish_envelope(command);
}

CommandPipelineStatus CommandPipeline::restore_live_sequence(
    CommandSequence next_sequence) noexcept {
  if (!open_) {
    return CommandPipelineStatus::Closed;
  }
  if (next_sequence == 0) {
    return CommandPipelineStatus::InvalidSequence;
  }

  next_live_sequence_ = next_sequence;
  sequence_exhausted_ = false;
  return CommandPipelineStatus::Ok;
}

CommandPipelineResult CommandPipeline::try_publish_envelope(
    const CommandEnvelope& command) noexcept {
  if (!open_) {
    return {CommandPipelineStatus::Closed};
  }
  if (control_->failure.persistence_failed.load(std::memory_order_acquire)) {
    return {CommandPipelineStatus::PersistenceFailed};
  }

  const std::uint64_t head =
      control_->head.value.load(std::memory_order_relaxed);
  const std::uint64_t tail =
      control_->tail.value.load(std::memory_order_acquire);
  if (head - tail == config_.capacity) {
    return {CommandPipelineStatus::Full};
  }

  CommandRingSlot& slot = slot_at(head);
  slot.command = command;
  slot.risk = {};
  slot.reserve = {};
  control_->head.value.store(head + 1, std::memory_order_release);
  return {CommandPipelineStatus::Ok, command.command_sequence, head + 1};
}

CommandPipelineResult CommandPipeline::copy_pending_for_persistence(
    std::uint32_t batch_offset, CommandEnvelope& command) const noexcept {
  if (!open_) {
    return {CommandPipelineStatus::Closed};
  }
  if (control_->failure.persistence_failed.load(std::memory_order_acquire)) {
    return {CommandPipelineStatus::PersistenceFailed};
  }

  const std::uint64_t durable =
      control_->durable.value.load(std::memory_order_relaxed);
  const std::uint64_t head =
      control_->head.value.load(std::memory_order_acquire);
  const std::uint64_t position = durable + batch_offset;
  if (position < durable || position >= head) {
    return {CommandPipelineStatus::Empty};
  }

  command = slot_at(position).command;
  return {CommandPipelineStatus::Ok, command.command_sequence, position};
}

CommandPipelineResult
CommandPipeline::publish_durable(std::uint32_t count) noexcept {
  if (!open_) {
    return {CommandPipelineStatus::Closed};
  }
  if (control_->failure.persistence_failed.load(std::memory_order_acquire)) {
    return {CommandPipelineStatus::PersistenceFailed};
  }
  if (count == 0) {
    return {CommandPipelineStatus::InvalidCount};
  }

  const std::uint64_t durable =
      control_->durable.value.load(std::memory_order_relaxed);
  const std::uint64_t head =
      control_->head.value.load(std::memory_order_acquire);
  if (count > head - durable) {
    return {CommandPipelineStatus::InvalidCount};
  }

  const std::uint64_t next = durable + count;
  const CommandSequence sequence = slot_at(next - 1).command.command_sequence;
  control_->durable.value.store(next, std::memory_order_release);
  return {CommandPipelineStatus::Ok, sequence, next};
}

void CommandPipeline::fail_persistence() noexcept {
  if (!open_) {
    return;
  }
  control_->failure.persistence_failed.store(true, std::memory_order_release);
}

CommandPipelineResult CommandPipeline::try_read_for_risk(
    CommandEnvelope& command) const noexcept {
  if (!open_) {
    return {CommandPipelineStatus::Closed};
  }

  const std::uint64_t risk =
      control_->risk_checked.value.load(std::memory_order_relaxed);
  const std::uint64_t durable =
      control_->durable.value.load(std::memory_order_acquire);
  if (risk == durable) {
    return {CommandPipelineStatus::Empty};
  }

  command = slot_at(risk).command;
  return {CommandPipelineStatus::Ok, command.command_sequence, risk};
}

CommandPipelineResult
CommandPipeline::publish_risk(const RiskResult& result) noexcept {
  if (!open_) {
    return {CommandPipelineStatus::Closed};
  }
  if (result.decision == CheckDecision::Pending) {
    return {CommandPipelineStatus::InvalidDecision};
  }

  const std::uint64_t risk =
      control_->risk_checked.value.load(std::memory_order_relaxed);
  const std::uint64_t durable =
      control_->durable.value.load(std::memory_order_acquire);
  if (risk == durable) {
    return {CommandPipelineStatus::Empty};
  }

  CommandRingSlot& slot = slot_at(risk);
  slot.risk = result;
  control_->risk_checked.value.store(risk + 1, std::memory_order_release);
  return {CommandPipelineStatus::Ok, slot.command.command_sequence, risk + 1};
}

CommandPipelineResult CommandPipeline::try_read_for_reserve(
    CommandEnvelope& command, RiskResult& risk_result) const noexcept {
  if (!open_) {
    return {CommandPipelineStatus::Closed};
  }

  const std::uint64_t reserve =
      control_->reserve_checked.value.load(std::memory_order_relaxed);
  const std::uint64_t risk =
      control_->risk_checked.value.load(std::memory_order_acquire);
  if (reserve == risk) {
    return {CommandPipelineStatus::Empty};
  }

  const CommandRingSlot& slot = slot_at(reserve);
  command = slot.command;
  risk_result = slot.risk;
  return {CommandPipelineStatus::Ok, command.command_sequence, reserve};
}

CommandPipelineResult
CommandPipeline::publish_reserve(const ReserveResult& result) noexcept {
  if (!open_) {
    return {CommandPipelineStatus::Closed};
  }
  if (result.decision == CheckDecision::Pending) {
    return {CommandPipelineStatus::InvalidDecision};
  }

  const std::uint64_t reserve =
      control_->reserve_checked.value.load(std::memory_order_relaxed);
  const std::uint64_t risk =
      control_->risk_checked.value.load(std::memory_order_acquire);
  if (reserve == risk) {
    return {CommandPipelineStatus::Empty};
  }

  CommandRingSlot& slot = slot_at(reserve);
  slot.reserve = result;
  control_->reserve_checked.value.store(reserve + 1,
                                        std::memory_order_release);
  return {CommandPipelineStatus::Ok, slot.command.command_sequence,
          reserve + 1};
}

CommandPipelineResult
CommandPipeline::try_consume(CommandRingSlot& slot) noexcept {
  if (!open_) {
    return {CommandPipelineStatus::Closed};
  }

  const std::uint64_t tail =
      control_->tail.value.load(std::memory_order_relaxed);
  const std::uint64_t reserve =
      control_->reserve_checked.value.load(std::memory_order_acquire);
  if (tail == reserve) {
    return {CommandPipelineStatus::Empty};
  }

  slot = slot_at(tail);
  control_->tail.value.store(tail + 1, std::memory_order_release);
  return {CommandPipelineStatus::Ok, slot.command.command_sequence, tail + 1};
}

CommandPipelineSnapshot CommandPipeline::snapshot() const noexcept {
  if (!open_) {
    return {};
  }
  return {control_->tail.value.load(std::memory_order_acquire),
          control_->reserve_checked.value.load(std::memory_order_acquire),
          control_->risk_checked.value.load(std::memory_order_acquire),
          control_->durable.value.load(std::memory_order_acquire),
          control_->head.value.load(std::memory_order_acquire),
          control_->failure.persistence_failed.load(std::memory_order_acquire)};
}

bool CommandPipeline::is_open() const noexcept {
  return open_;
}

const CommandPipelineConfig& CommandPipeline::config() const noexcept {
  return config_;
}

CommandRingSlot& CommandPipeline::slot_at(std::uint64_t position) noexcept {
  return slots_[position % config_.capacity];
}

const CommandRingSlot&
CommandPipeline::slot_at(std::uint64_t position) const noexcept {
  return slots_[position % config_.capacity];
}

} // namespace fexma::matcher
