/**
 * @file test_wal_slider.cpp
 * @brief Contract tests for generic synchronous WAL slider mechanics.
 */

#include <fexma/wal/slider.hpp>

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>

using namespace fexma::wal;

namespace {

using Payload = std::array<std::byte, 8>;

[[nodiscard]] Payload payload(std::uint64_t value) noexcept {
  Payload result{};
  for (std::size_t index = 0; index < result.size(); ++index) {
    result[index] =
        static_cast<std::byte>((value >> (index * 8u)) & 0xffu);
  }
  return result;
}

class RecordingModule final {
public:
  explicit RecordingModule(const Frontier& published,
                           Position fail_at = static_cast<Position>(-1)) noexcept
      : published_(&published), fail_at_(fail_at) {}

  [[nodiscard]] bool process(const RecordView& record) noexcept {
    if (record.position == fail_at_) return false;
    positions_[count_] = record.position;
    published_on_entry_[count_] = published_->acquire();
    ++count_;
    return true;
  }

  void allow_all() noexcept { fail_at_ = static_cast<Position>(-1); }
  [[nodiscard]] std::size_t count() const noexcept { return count_; }
  [[nodiscard]] Position position(std::size_t index) const noexcept {
    return positions_[index];
  }
  [[nodiscard]] Position published_on_entry(std::size_t index) const noexcept {
    return published_on_entry_[index];
  }

private:
  const Frontier* published_{};
  Position fail_at_{};
  std::array<Position, 8> positions_{};
  std::array<Position, 8> published_on_entry_{};
  std::size_t count_{};
};

template <class T>
concept FrontierPublisher = requires(T& frontier, Position end) {
  { frontier.publish(end) } noexcept -> std::same_as<bool>;
};

static_assert(!std::is_polymorphic_v<RecordingModule>);
static_assert(!std::is_copy_constructible_v<Frontier>);
static_assert(FrontierPublisher<Frontier>);
static_assert(!FrontierPublisher<const Frontier>);

[[nodiscard]] bool frontier_is_monotonic_and_resettable_quiescent() noexcept {
  Frontier frontier(1);
  if (frontier.acquire() != 1 || !frontier.publish(3) ||
      frontier.acquire() != 3 || frontier.publish(2) ||
      frontier.acquire() != 3) {
    return false;
  }
  frontier.reset_quiescent(2);
  return frontier.acquire() == 2;
}

[[nodiscard]] bool open_tape(RecordTape& tape, std::uint32_t capacity = 4) {
  return tape.open({static_cast<std::uint32_t>(sizeof(Payload)), capacity,
                    default_alignment, 10})
      .ok();
}

[[nodiscard]] bool publish(RecordTape& tape, std::uint64_t value) {
  const Payload bytes = payload(value);
  return tape.try_publish(std::span<const std::byte>{bytes}).ok();
}

[[nodiscard]] bool processes_available_range_in_order() {
  RecordTape tape;
  if (!open_tape(tape) || !publish(tape, 1) || !publish(tape, 2) ||
      !publish(tape, 3)) {
    return false;
  }

  Frontier progress;
  RecordingModule module(progress);
  Slider slider(tape, progress, module);

  const SliderResult result = slider.process_available();
  if (result.status != SliderStatus::Processed || result.processed_count != 3 ||
      result.current != 3 || slider.current() != 3 ||
      progress.acquire() != 3 || module.count() != 3) {
    return false;
  }
  for (Position position = 0; position < 3; ++position) {
    if (module.position(static_cast<std::size_t>(position)) != position ||
        module.published_on_entry(static_cast<std::size_t>(position)) !=
            position) {
      return false;
    }
  }

  const SliderResult empty = slider.process_available();
  return empty.status == SliderStatus::Empty && empty.processed_count == 0 &&
         module.count() == 3;
}

[[nodiscard]] bool respects_upstream_and_retries_module_failure() {
  RecordTape tape;
  if (!open_tape(tape) || !publish(tape, 1) || !publish(tape, 2) ||
      !publish(tape, 3)) {
    return false;
  }

  Frontier upstream(2);
  Frontier own;
  RecordingModule module(own, 1);
  Slider slider(tape, upstream, own, module);

  const SliderResult failed = slider.process_available();
  if (failed.status != SliderStatus::ModuleFailed ||
      failed.processed_count != 1 || failed.current != 1 ||
      slider.current() != 1 || own.acquire() != 1 ||
      module.count() != 1 || module.position(0) != 0) {
    return false;
  }

  module.allow_all();
  const SliderResult retried = slider.process_available();
  if (retried.status != SliderStatus::Processed ||
      retried.processed_count != 1 || retried.current != 2 ||
      module.count() != 2 || module.position(1) != 1 ||
      own.acquire() != 2) {
    return false;
  }

  if (!upstream.publish(3)) return false;
  const SliderResult last = slider.process_available();
  return last.status == SliderStatus::Processed &&
         last.processed_count == 1 && last.current == 3 &&
         module.position(2) == 2 && own.acquire() == 3;
}

[[nodiscard]] bool rejects_regressed_upstream() {
  RecordTape tape;
  if (!open_tape(tape) || !publish(tape, 1)) return false;

  Frontier regressed_upstream;
  Frontier regressed_own(1);
  RecordingModule regressed_module(regressed_own);
  Slider regressed(tape, regressed_upstream, regressed_own, regressed_module);
  return regressed.process_available().status ==
             SliderStatus::UpstreamRegression &&
         regressed_module.count() == 0 && regressed_own.acquire() == 1;
}

[[nodiscard]] bool reports_unavailable_views_without_publication() {
  RecordTape tape;
  if (!open_tape(tape) || !publish(tape, 1)) return false;

  Frontier upstream(1);
  Frontier own;
  RecordingModule module(own);
  Slider reclaimed(tape, upstream, own, module);
  if (tape.reclaim(1) != ReclaimStatus::Ok) return false;

  const SliderResult reclaimed_result = reclaimed.process_available();
  if (reclaimed_result.status != SliderStatus::ViewUnavailable ||
      reclaimed_result.view_status != ViewStatus::Reclaimed ||
      reclaimed_result.processed_count != 0 || own.acquire() != 0) {
    return false;
  }

  RecordTape second;
  if (!open_tape(second)) return false;
  Frontier ahead(1);
  Frontier second_own;
  RecordingModule second_module(second_own);
  Slider unpublished(second, ahead, second_own, second_module);
  const SliderResult unpublished_result = unpublished.process_available();
  return unpublished_result.status == SliderStatus::ViewUnavailable &&
         unpublished_result.view_status == ViewStatus::Unpublished &&
         second_own.acquire() == 0 && second_module.count() == 0;
}

} // namespace

int main() {
  if (!frontier_is_monotonic_and_resettable_quiescent()) return 1;
  if (!processes_available_range_in_order()) return 2;
  if (!respects_upstream_and_retries_module_failure()) return 3;
  if (!rejects_regressed_upstream()) return 4;
  if (!reports_unavailable_views_without_publication()) return 5;
  return 0;
}
