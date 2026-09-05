#pragma once

#include <fexma/matcher/types.hpp>

#include <atomic>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <tuple>
#include <type_traits>

namespace fexma::matcher::demo {

using TractPosition = std::uint64_t;

template <class Module>
concept TractModule =
    requires(Module& module, const CommandEnvelope& command) {
      { module.process(command) } noexcept;
    };

template <class Source>
concept TractSource =
    requires(const Source& source, TractPosition position) {
      { source.at(position) } noexcept
          -> std::same_as<const CommandEnvelope&>;
    };

class Frontier final {
public:
  explicit Frontier(TractPosition initial = 0) noexcept : value_(initial) {}

  Frontier(const Frontier&) = delete;
  Frontier& operator=(const Frontier&) = delete;

  [[nodiscard]] TractPosition acquire() const noexcept {
    return value_.load(std::memory_order_acquire);
  }

  [[nodiscard]] bool publish(TractPosition position) noexcept {
    const TractPosition current = value_.load(std::memory_order_relaxed);
    if (position < current) {
      return false;
    }
    value_.store(position, std::memory_order_release);
    return true;
  }

private:
  std::atomic<TractPosition> value_{};
};

enum class ConsumeStatus : std::uint8_t {
  Processed,
  Empty,
  FrontierRegression
};

struct ConsumeResult {
  ConsumeStatus status{ConsumeStatus::Empty};
  TractPosition processed_through{};
  std::uint64_t processed_count{};

  [[nodiscard]] bool ok() const noexcept {
    return status == ConsumeStatus::Processed || status == ConsumeStatus::Empty;
  }
};

template <TractModule Module, TractSource Source>
class TractConsumer final {
public:
  TractConsumer(Module& module, const Source& source, Frontier& upstream,
                Frontier& downstream,
                TractPosition initial_position = 0) noexcept
      : module_(module), source_(source), upstream_(upstream),
        downstream_(downstream), current_(initial_position) {}

  [[nodiscard]] ConsumeResult process_available() noexcept {
    const TractPosition available = upstream_.acquire();
    if (available < current_) {
      return {ConsumeStatus::FrontierRegression, current_, 0};
    }
    if (available == current_) {
      return {ConsumeStatus::Empty, current_, 0};
    }

    const TractPosition first = current_ + 1;
    while (current_ < available) {
      module_.process(source_.at(current_ + 1));
      ++current_;
    }

    if (!downstream_.publish(current_)) {
      return {ConsumeStatus::FrontierRegression, current_, current_ - first + 1};
    }
    return {ConsumeStatus::Processed, current_, current_ - first + 1};
  }

  [[nodiscard]] TractPosition current() const noexcept { return current_; }

private:
  Module& module_;
  const Source& source_;
  Frontier& upstream_;
  Frontier& downstream_;
  TractPosition current_{};
};

template <TractModule... Modules>
struct StaticTract final {
  using ModuleTypes = std::tuple<Modules...>;
  static constexpr std::size_t module_count = sizeof...(Modules);
};

} // namespace fexma::matcher::demo
