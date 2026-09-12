#pragma once

/**
 * @file slider.hpp
 * @brief Generic synchronous mechanics for a stage over RecordTape records.
 */

#include <fexma/wal/record_tape.hpp>

#include <array>
#include <atomic>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace fexma::wal {

class alignas(64) Frontier final {
public:
  explicit Frontier(Position initial = 0) noexcept : value_(initial) {}

  Frontier(const Frontier&) = delete;
  Frontier& operator=(const Frontier&) = delete;
  Frontier(Frontier&&) = delete;
  Frontier& operator=(Frontier&&) = delete;

  [[nodiscard]] Position acquire() const noexcept {
    return value_.load(std::memory_order_acquire);
  }

  [[nodiscard]] bool publish(Position end) noexcept {
    const Position current = value_.load(std::memory_order_relaxed);
    if (end < current) return false;
    value_.store(end, std::memory_order_release);
    return true;
  }

  // Cold-path initialization: no reader or writer may be active.
  void reset_quiescent(Position initial) noexcept {
    value_.store(initial, std::memory_order_relaxed);
  }

private:
  static constexpr std::size_t cache_line_size = 64;
  static_assert(std::atomic<Position>::is_always_lock_free);

  std::atomic<Position> value_{};
  std::array<std::byte,
             cache_line_size - sizeof(std::atomic<Position>)>
      padding_{};
};

static_assert(sizeof(Frontier) == 64);

class RecordTapeHeadProgress final {
public:
  explicit RecordTapeHeadProgress(const RecordTape& tape) noexcept
      : tape_(&tape) {}

  [[nodiscard]] Position acquire() const noexcept { return tape_->head(); }

private:
  const RecordTape* tape_{};
};

struct PositionRange {
  Position begin{};
  Position end{};
};

struct AvailableRangeAcquire final {
  [[nodiscard]] PositionRange acquire(Position current,
                                      Position available_end) const noexcept {
    return {current, available_end};
  }
};

class BoundedRangeAcquire final {
public:
  explicit BoundedRangeAcquire(Position maximum_count = 0) noexcept
      : maximum_count_(maximum_count) {}

  void set_maximum_count(Position maximum_count) noexcept {
    maximum_count_ = maximum_count;
  }

  [[nodiscard]] PositionRange acquire(Position current,
                                      Position available_end) const noexcept {
    if (available_end <= current) return {current, current};
    const Position available_count = available_end - current;
    const Position count =
        available_count < maximum_count_ ? available_count : maximum_count_;
    return {current, current + count};
  }

private:
  Position maximum_count_{};
};

enum class PublishDecision : std::uint8_t {
  Hold,
  Publish,
  Failed
};

struct OnePositionPublish final {
  template <class Module>
  [[nodiscard]] PublishDecision after_process(Module&,
                                              Position) const noexcept {
    return PublishDecision::Publish;
  }

  template <class Module>
  [[nodiscard]] PublishDecision after_range(Module&, Position,
                                            Position) const noexcept {
    return PublishDecision::Hold;
  }
};

enum class SliderStatus : std::uint8_t {
  Processed,
  Empty,
  UpstreamRegression,
  InvalidRange,
  ViewUnavailable,
  ModuleFailed,
  PublishFailed
};

struct SliderResult {
  SliderStatus status{SliderStatus::Empty};
  Position current{};
  std::uint64_t processed_count{};
  ViewStatus view_status{ViewStatus::Ok};

  [[nodiscard]] bool ok() const noexcept {
    return status == SliderStatus::Processed || status == SliderStatus::Empty;
  }
};

template <class Source>
concept SliderViewSource =
    requires(const Source& source, Position position) {
      { source.try_view(position) } noexcept -> std::same_as<AccessResult>;
    };

template <class Upstream>
concept SliderUpstreamProgress = requires(const Upstream& upstream) {
  { upstream.acquire() } noexcept -> std::same_as<Position>;
};

template <class Module>
concept SliderModule = requires(Module& module, const RecordView& record) {
  { module.process(record) } noexcept -> std::same_as<bool>;
};

template <class Policy>
concept SliderAcquirePolicy =
    requires(const Policy& policy, Position current, Position available_end) {
      { policy.acquire(current, available_end) } noexcept
          -> std::same_as<PositionRange>;
    };

template <class Policy, class Module>
concept SliderPublishPolicy =
    requires(Policy& policy, Module& module, Position begin, Position end) {
      { policy.after_process(module, end) } noexcept
          -> std::same_as<PublishDecision>;
      { policy.after_range(module, begin, end) } noexcept
          -> std::same_as<PublishDecision>;
    };

template <SliderViewSource ViewSource, SliderUpstreamProgress UpstreamProgress,
          SliderModule Module,
          SliderAcquirePolicy AcquirePolicy = AvailableRangeAcquire,
          class PublishPolicy = OnePositionPublish>
  requires SliderPublishPolicy<PublishPolicy, Module>
class Slider final {
public:
  Slider(const ViewSource& source, const UpstreamProgress& upstream,
         Frontier& own, Module& module,
         AcquirePolicy acquire_policy = {},
         PublishPolicy publish_policy = {}) noexcept
      : source_(source), upstream_(upstream), own_(own), module_(module),
        acquire_policy_(std::move(acquire_policy)),
        publish_policy_(std::move(publish_policy)) {}

  [[nodiscard]] SliderResult process_available() noexcept {
    Position current = own_.acquire();

    const Position available_end = upstream_.acquire();
    if (available_end < current) {
      return {SliderStatus::UpstreamRegression, current, 0};
    }

    const PositionRange range =
        acquire_policy_.acquire(current, available_end);
    if (range.begin != current || range.end < range.begin ||
        range.end > available_end) {
      return {SliderStatus::InvalidRange, current, 0};
    }
    if (range.begin == range.end) {
      return {SliderStatus::Empty, current, 0};
    }

    const Position first = current;
    std::uint64_t processed_count = 0;
    while (current < range.end) {
      const AccessResult access = source_.try_view(current);
      if (!access.ok()) {
        return {SliderStatus::ViewUnavailable, current, processed_count,
                access.status};
      }
      if (!module_.process(access.record)) {
        return {SliderStatus::ModuleFailed, current, processed_count};
      }

      ++current;
      ++processed_count;
      if (!apply_publish_decision(
              publish_policy_.after_process(module_, current), current)) {
        return {SliderStatus::PublishFailed, current, processed_count};
      }
    }

    if (!apply_publish_decision(
            publish_policy_.after_range(module_, first, current), current)) {
      return {SliderStatus::PublishFailed, current, processed_count};
    }
    return {SliderStatus::Processed, current, processed_count};
  }

  [[nodiscard]] Position current() const noexcept { return own_.acquire(); }
  // Cold-path initialization: process_available() must not be active.
  void reset_quiescent(Position initial) noexcept {
    own_.reset_quiescent(initial);
  }

  [[nodiscard]] AcquirePolicy& acquire_policy() noexcept {
    return acquire_policy_;
  }

private:
  [[nodiscard]] bool
  apply_publish_decision(PublishDecision decision, Position current) noexcept {
    if (decision == PublishDecision::Failed) return false;
    return decision == PublishDecision::Hold || own_.publish(current);
  }

  const ViewSource& source_;
  const UpstreamProgress& upstream_;
  Frontier& own_;
  Module& module_;
  [[no_unique_address]] AcquirePolicy acquire_policy_;
  [[no_unique_address]] PublishPolicy publish_policy_;
};

} // namespace fexma::wal
