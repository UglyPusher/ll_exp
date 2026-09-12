#pragma once

/**
 * @file slider.hpp
 * @brief Generic synchronous mechanics for a stage over RecordTape records.
 */

#include <fexma/wal/record_tape.hpp>
#include <fexma/wal/record_tape_types.hpp>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

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

enum class SliderStatus : std::uint8_t {
  Processed,
  Empty,
  UpstreamRegression,
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

template <class Module>
class Slider final {
public:
  Slider(const RecordTape& source, Frontier& own, Module& module) noexcept
      : source_(source), own_(own), module_(module) {}

  Slider(const RecordTape& source, const Frontier& upstream, Frontier& own,
         Module& module) noexcept
      : source_(source), upstream_(&upstream), own_(own), module_(module) {}

  [[nodiscard]] SliderResult process_available() noexcept {
    Position current = own_.acquire();

    const Position available_end =
        upstream_ == nullptr ? source_.head() : upstream_->acquire();
    if (available_end < current) {
      return {SliderStatus::UpstreamRegression, current, 0};
    }
    if (available_end == current) {
      return {SliderStatus::Empty, current, 0};
    }

    std::uint64_t processed_count = 0;
    while (current < available_end) {
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
      if (!own_.publish(current)) {
        return {SliderStatus::PublishFailed, current, processed_count};
      }
    }
    return {SliderStatus::Processed, current, processed_count};
  }

  [[nodiscard]] Position current() const noexcept { return own_.acquire(); }
  // Cold-path initialization: process_available() must not be active.
  void reset_quiescent(Position initial) noexcept {
    own_.reset_quiescent(initial);
  }

private:
  const RecordTape& source_;
  const Frontier* upstream_{};
  Frontier& own_;
  Module& module_;
};

} // namespace fexma::wal
