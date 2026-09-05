/**
 * @file test_command_pipeline.cpp
 * @brief Contract and concurrent stress tests for the Command SPMC pipeline.
 */
#include <fexma/matcher/command_pipeline.hpp>
#include "../demo/tract_consumer.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <thread>
#include <type_traits>

using namespace fexma::matcher;

namespace {

[[nodiscard]] CommandWalPayload payload(std::uint64_t value) noexcept {
  return {value, Command{ShutdownCommand{}}};
}

[[nodiscard]] CommandEnvelope replay_command(
    CommandSequence sequence, std::uint64_t value) noexcept {
  return {sequence, payload(value)};
}

class DemoArraySource final {
public:
  explicit DemoArraySource(
      const std::array<CommandEnvelope, 3>& commands) noexcept
      : commands_(commands) {}

  [[nodiscard]] const CommandEnvelope&
  at(demo::TractPosition position) const noexcept {
    return commands_[static_cast<std::size_t>(position - 1)];
  }

private:
  const std::array<CommandEnvelope, 3>& commands_;
};

class DemoRecordingModule final {
public:
  void process(const CommandEnvelope& command) noexcept {
    observed_[count_] = command.command_sequence;
    ++count_;
  }

  [[nodiscard]] std::size_t count() const noexcept { return count_; }

  [[nodiscard]] std::uint64_t observed(std::size_t index) const noexcept {
    return observed_[index];
  }

private:
  std::array<std::uint64_t, 3> observed_{};
  std::size_t count_{};
};

class DemoMissingProcess final {};

class DemoThrowingProcess final {
public:
  void process(const CommandEnvelope&) {}
};

static_assert(demo::TractModule<DemoRecordingModule>);
static_assert(!demo::TractModule<DemoMissingProcess>);
static_assert(!demo::TractModule<DemoThrowingProcess>);
static_assert(!std::is_polymorphic_v<DemoRecordingModule>);
using TestDemoTract =
    demo::StaticTract<DemoRecordingModule, DemoRecordingModule>;
static_assert(TestDemoTract::module_count == 2);

[[nodiscard]] std::array<CommandEnvelope, 3> demo_commands() noexcept {
  return {{{1, CommandWalPayload{}},
           {2, CommandWalPayload{}},
           {3, CommandWalPayload{}}}};
}

[[nodiscard]] bool compile_time_consumer_obeys_frontiers() noexcept {
  demo::Frontier monotonic;
  if (!monotonic.publish(3) || monotonic.publish(2) ||
      monotonic.acquire() != 3) {
    return false;
  }

  const auto commands = demo_commands();
  const DemoArraySource source(commands);
  DemoRecordingModule module;
  demo::Frontier upstream;
  demo::Frontier downstream;
  demo::TractConsumer consumer(module, source, upstream, downstream);

  if (!upstream.publish(2)) {
    return false;
  }
  const demo::ConsumeResult first = consumer.process_available();
  if (first.status != demo::ConsumeStatus::Processed ||
      first.processed_count != 2 || first.processed_through != 2 ||
      consumer.current() != 2 || downstream.acquire() != 2 ||
      module.count() != 2 || module.observed(0) != 1 ||
      module.observed(1) != 2) {
    return false;
  }

  const demo::ConsumeResult empty = consumer.process_available();
  if (empty.status != demo::ConsumeStatus::Empty || module.count() != 2) {
    return false;
  }

  if (!upstream.publish(3)) {
    return false;
  }
  const demo::ConsumeResult last = consumer.process_available();
  return last.status == demo::ConsumeStatus::Processed &&
         last.processed_count == 1 && last.processed_through == 3 &&
         module.count() == 3 && module.observed(2) == 3 &&
         downstream.acquire() == 3;
}

[[nodiscard]] bool opens_only_valid_configuration() {
  CommandPipeline pipeline;
  pipeline.fail_persistence();
  return pipeline.restore_live_sequence(1) == CommandPipelineStatus::Closed &&
         pipeline.try_replay(replay_command(1, 1)).status ==
             CommandPipelineStatus::Closed &&
         pipeline.open({}) == CommandPipelineStatus::InvalidConfig &&
         pipeline.open({4, 0}) == CommandPipelineStatus::InvalidConfig &&
         pipeline.open({4, 10}) == CommandPipelineStatus::Ok &&
         pipeline.restore_live_sequence(0) ==
             CommandPipelineStatus::InvalidSequence &&
         pipeline.open({4, 10}) == CommandPipelineStatus::AlreadyOpen;
}

[[nodiscard]] bool preserves_replay_identity_and_live_sequence() {
  CommandPipeline pipeline;
  if (pipeline.open({5, 100}) != CommandPipelineStatus::Ok ||
      pipeline.try_replay(replay_command(0, 1)).status !=
          CommandPipelineStatus::InvalidSequence ||
      pipeline.try_publish(payload(100)).sequence != 100 ||
      pipeline.try_replay(replay_command(7, 7)).sequence != 7 ||
      pipeline.try_replay(replay_command(8, 8)).sequence != 8 ||
      pipeline.try_publish(payload(101)).sequence != 101 ||
      pipeline.restore_live_sequence(500) != CommandPipelineStatus::Ok ||
      pipeline.try_publish(payload(500)).sequence != 500 ||
      pipeline.try_replay(replay_command(9, 9)).status !=
          CommandPipelineStatus::Full) {
    return false;
  }

  constexpr CommandSequence expected_sequences[] = {100, 7, 8, 101, 500};
  constexpr std::uint64_t expected_clients[] = {100, 7, 8, 101, 500};
  CommandEnvelope command{};
  for (std::size_t index = 0; index < std::size(expected_sequences); ++index) {
    if (pipeline.copy_pending_for_persistence(
            static_cast<std::uint32_t>(index), command).sequence !=
            expected_sequences[index] ||
        command.command_sequence != expected_sequences[index]) {
      return false;
    }
  }
  if (pipeline.publish_durable(5).sequence != 500) {
    return false;
  }

  RiskResult risk{};
  CommandRingSlot slot{};
  for (std::size_t index = 0; index < std::size(expected_sequences); ++index) {
    const CommandSequence expected_sequence = expected_sequences[index];
    if (pipeline.try_read_for_risk(command).sequence != expected_sequence ||
        command.command_sequence != expected_sequence ||
        pipeline.publish_risk({CheckDecision::Accepted, 1}).sequence !=
            expected_sequence ||
        pipeline.try_read_for_reserve(command, risk).sequence !=
            expected_sequence ||
        pipeline.publish_reserve({CheckDecision::Accepted, 2}).sequence !=
            expected_sequence ||
        pipeline.try_consume(slot).sequence != expected_sequence ||
        slot.command.command_sequence != expected_sequence ||
        slot.command.payload.client_id != expected_clients[index]) {
      return false;
    }
  }

  const CommandPipelineSnapshot state = pipeline.snapshot();
  return state.tail == 5 && state.reserve_checked == 5 &&
         state.risk_checked == 5 && state.durable == 5 && state.head == 5;
}

[[nodiscard]] bool preserves_stage_order_and_batch_visibility() {
  CommandPipeline pipeline;
  if (pipeline.open({4, 10}) != CommandPipelineStatus::Ok) {
    return false;
  }

  if (pipeline.try_publish(payload(1)).sequence != 10 ||
      pipeline.try_publish(payload(2)).sequence != 11) {
    return false;
  }

  CommandEnvelope command{};
  if (pipeline.try_read_for_risk(command).status !=
      CommandPipelineStatus::Empty) {
    return false;
  }
  if (pipeline.copy_pending_for_persistence(0, command).sequence != 10 ||
      command.payload.client_id != 1 ||
      pipeline.copy_pending_for_persistence(1, command).sequence != 11 ||
      command.payload.client_id != 2 ||
      pipeline.copy_pending_for_persistence(2, command).status !=
          CommandPipelineStatus::Empty) {
    return false;
  }
  if (pipeline.publish_durable(2).frontier != 2) {
    return false;
  }

  RiskResult risk{};
  if (!pipeline.try_read_for_risk(command).ok() ||
      command.command_sequence != 10 ||
      !pipeline.publish_risk({CheckDecision::Accepted, 101}).ok()) {
    return false;
  }
  if (pipeline.try_read_for_reserve(command, risk).sequence != 10 ||
      risk.decision != CheckDecision::Accepted || risk.reason_code != 101 ||
      !pipeline.publish_reserve({CheckDecision::Accepted, 201}).ok()) {
    return false;
  }

  CommandRingSlot slot{};
  if (pipeline.try_consume(slot).sequence != 10 ||
      slot.command.payload.client_id != 1 ||
      slot.risk.reason_code != 101 || slot.reserve.reason_code != 201) {
    return false;
  }

  const CommandPipelineSnapshot state = pipeline.snapshot();
  return state.tail == 1 && state.reserve_checked == 1 &&
         state.risk_checked == 1 && state.durable == 2 && state.head == 2;
}

[[nodiscard]] bool wraps_only_after_tail_releases_capacity() {
  CommandPipeline pipeline;
  if (pipeline.open({2, 1}) != CommandPipelineStatus::Ok ||
      !pipeline.try_publish(payload(1)).ok() ||
      !pipeline.try_publish(payload(2)).ok() ||
      pipeline.try_publish(payload(3)).status != CommandPipelineStatus::Full ||
      !pipeline.publish_durable(2).ok()) {
    return false;
  }

  CommandEnvelope command{};
  RiskResult risk{};
  CommandRingSlot slot{};
  if (!pipeline.try_read_for_risk(command).ok() ||
      !pipeline.publish_risk({CheckDecision::Accepted, 1}).ok() ||
      !pipeline.try_read_for_reserve(command, risk).ok() ||
      !pipeline.publish_reserve({CheckDecision::Accepted, 2}).ok() ||
      pipeline.try_publish(payload(3)).status != CommandPipelineStatus::Full ||
      !pipeline.try_consume(slot).ok()) {
    return false;
  }

  const CommandPipelineResult wrapped = pipeline.try_publish(payload(3));
  return wrapped.ok() && wrapped.sequence == 3 &&
         slot.command.command_sequence == 1;
}

[[nodiscard]] bool fails_closed_but_drains_durable_prefix() {
  CommandPipeline pipeline;
  if (pipeline.open({4, 1}) != CommandPipelineStatus::Ok ||
      !pipeline.try_publish(payload(1)).ok() ||
      !pipeline.try_publish(payload(2)).ok() ||
      !pipeline.try_publish(payload(3)).ok() ||
      !pipeline.publish_durable(2).ok()) {
    return false;
  }

  pipeline.fail_persistence();
  CommandEnvelope command{};
  if (pipeline.try_publish(payload(4)).status !=
          CommandPipelineStatus::PersistenceFailed ||
      pipeline.try_replay(replay_command(4, 4)).status !=
          CommandPipelineStatus::PersistenceFailed ||
      pipeline.copy_pending_for_persistence(0, command).status !=
          CommandPipelineStatus::PersistenceFailed ||
      pipeline.publish_durable(1).status !=
          CommandPipelineStatus::PersistenceFailed) {
    return false;
  }

  RiskResult risk{};
  CommandRingSlot slot{};
  for (std::uint32_t index = 0; index < 2; ++index) {
    if (!pipeline.try_read_for_risk(command).ok() ||
        !pipeline.publish_risk({CheckDecision::Accepted, index}).ok() ||
        !pipeline.try_read_for_reserve(command, risk).ok() ||
        !pipeline.publish_reserve({CheckDecision::Accepted, index}).ok() ||
        !pipeline.try_consume(slot).ok()) {
      return false;
    }
  }

  const CommandPipelineSnapshot state = pipeline.snapshot();
  return pipeline.try_read_for_risk(command).status ==
             CommandPipelineStatus::Empty &&
         state.tail == 2 && state.reserve_checked == 2 &&
         state.risk_checked == 2 && state.durable == 2 && state.head == 3 &&
         state.persistence_failed;
}

[[nodiscard]] bool rejects_invalid_transitions_and_sequence_overflow() {
  CommandPipeline pipeline;
  if (pipeline.open({2, (std::numeric_limits<CommandSequence>::max)()}) !=
          CommandPipelineStatus::Ok ||
      !pipeline.try_publish(payload(1)).ok() ||
      pipeline.try_publish(payload(2)).status !=
          CommandPipelineStatus::SequenceExhausted ||
      pipeline.publish_durable(0).status !=
          CommandPipelineStatus::InvalidCount ||
      pipeline.publish_durable(2).status !=
          CommandPipelineStatus::InvalidCount ||
      !pipeline.publish_durable(1).ok()) {
    return false;
  }

  return pipeline.publish_risk({CheckDecision::Pending, 0}).status ==
             CommandPipelineStatus::InvalidDecision &&
         pipeline.publish_reserve({CheckDecision::Pending, 0}).status ==
             CommandPipelineStatus::InvalidDecision;
}

[[nodiscard]] bool concurrent_five_role_stress() {
  constexpr std::uint64_t command_count = 100'000;
  CommandPipeline pipeline;
  if (pipeline.open({128, 1}) != CommandPipelineStatus::Ok) {
    return false;
  }

  std::atomic<bool> failed{};

  std::thread producer([&] {
    for (std::uint64_t index = 0; index < command_count && !failed; ++index) {
      while (true) {
        const CommandPipelineResult published =
            pipeline.try_publish(payload(index + 1));
        if (published.ok()) {
          if (published.sequence != index + 1) {
            failed.store(true, std::memory_order_relaxed);
          }
          break;
        }
        if (published.status != CommandPipelineStatus::Full) {
          failed.store(true, std::memory_order_relaxed);
          break;
        }
        std::this_thread::yield();
      }
    }
  });

  std::thread persistence([&] {
    CommandEnvelope command{};
    for (std::uint64_t index = 0; index < command_count && !failed; ++index) {
      while (true) {
        const CommandPipelineResult pending =
            pipeline.copy_pending_for_persistence(0, command);
        if (pending.ok()) {
          if (pending.sequence != index + 1 ||
              command.payload.client_id != index + 1 ||
              !pipeline.publish_durable(1).ok()) {
            failed.store(true, std::memory_order_relaxed);
          }
          break;
        }
        if (pending.status != CommandPipelineStatus::Empty) {
          failed.store(true, std::memory_order_relaxed);
          break;
        }
        std::this_thread::yield();
      }
    }
  });

  std::thread risk_stage([&] {
    CommandEnvelope command{};
    for (std::uint64_t index = 0; index < command_count && !failed; ++index) {
      while (true) {
        const CommandPipelineResult read = pipeline.try_read_for_risk(command);
        if (read.ok()) {
          if (read.sequence != index + 1 ||
              !pipeline.publish_risk(
                   {CheckDecision::Accepted,
                    static_cast<std::uint32_t>(index)}).ok()) {
            failed.store(true, std::memory_order_relaxed);
          }
          break;
        }
        if (read.status != CommandPipelineStatus::Empty) {
          failed.store(true, std::memory_order_relaxed);
          break;
        }
        std::this_thread::yield();
      }
    }
  });

  std::thread reserve_stage([&] {
    CommandEnvelope command{};
    RiskResult risk{};
    for (std::uint64_t index = 0; index < command_count && !failed; ++index) {
      while (true) {
        const CommandPipelineResult read =
            pipeline.try_read_for_reserve(command, risk);
        if (read.ok()) {
          if (read.sequence != index + 1 ||
              risk.reason_code != static_cast<std::uint32_t>(index) ||
              !pipeline.publish_reserve(
                   {CheckDecision::Accepted,
                    static_cast<std::uint32_t>(index)}).ok()) {
            failed.store(true, std::memory_order_relaxed);
          }
          break;
        }
        if (read.status != CommandPipelineStatus::Empty) {
          failed.store(true, std::memory_order_relaxed);
          break;
        }
        std::this_thread::yield();
      }
    }
  });

  std::thread matcher_stage([&] {
    CommandRingSlot slot{};
    for (std::uint64_t index = 0; index < command_count && !failed; ++index) {
      while (true) {
        const CommandPipelineResult consumed = pipeline.try_consume(slot);
        if (consumed.ok()) {
          if (consumed.sequence != index + 1 ||
              slot.command.payload.client_id != index + 1 ||
              slot.risk.reason_code != static_cast<std::uint32_t>(index) ||
              slot.reserve.reason_code != static_cast<std::uint32_t>(index)) {
            failed.store(true, std::memory_order_relaxed);
          }
          break;
        }
        if (consumed.status != CommandPipelineStatus::Empty) {
          failed.store(true, std::memory_order_relaxed);
          break;
        }
        std::this_thread::yield();
      }
    }
  });

  producer.join();
  persistence.join();
  risk_stage.join();
  reserve_stage.join();
  matcher_stage.join();

  const CommandPipelineSnapshot state = pipeline.snapshot();
  return !failed && state.tail == command_count &&
         state.reserve_checked == command_count &&
         state.risk_checked == command_count &&
         state.durable == command_count && state.head == command_count;
}

} // namespace

int main() {
  if (!compile_time_consumer_obeys_frontiers()) {
    return EXIT_FAILURE;
  }
  if (!opens_only_valid_configuration()) {
    return EXIT_FAILURE;
  }
  if (!preserves_stage_order_and_batch_visibility()) {
    return EXIT_FAILURE;
  }
  if (!preserves_replay_identity_and_live_sequence()) {
    return EXIT_FAILURE;
  }
  if (!wraps_only_after_tail_releases_capacity()) {
    return EXIT_FAILURE;
  }
  if (!fails_closed_but_drains_durable_prefix()) {
    return EXIT_FAILURE;
  }
  if (!rejects_invalid_transitions_and_sequence_overflow()) {
    return EXIT_FAILURE;
  }
  if (!concurrent_five_role_stress()) {
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
