/**
 * @file test_wal_slider.cpp
 * @brief Contract tests for generic synchronous WAL slider mechanics.
 */

#include <fexma/wal/slider.hpp>

#include <array>
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
  explicit RecordingModule(const Progress::Reader& published,
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
  const Progress::Reader* published_{};
  Position fail_at_{};
  std::array<Position, 8> positions_{};
  std::array<Position, 8> published_on_entry_{};
  std::size_t count_{};
};

class MissingProcess final {};
class ThrowingProcess final {
public:
  bool process(const RecordView&) { return true; }
};

static_assert(SliderModule<RecordingModule>);
static_assert(!SliderModule<MissingProcess>);
static_assert(!SliderModule<ThrowingProcess>);
static_assert(!std::is_polymorphic_v<RecordingModule>);
static_assert(!std::is_copy_constructible_v<Progress::Writer>);

[[nodiscard]] bool open_core(WalCore& core, std::uint32_t capacity = 4) {
  return core.open({static_cast<std::uint32_t>(sizeof(Payload)), capacity,
                    default_alignment, 10})
      .ok();
}

[[nodiscard]] bool publish(WalCore& core, std::uint64_t value) {
  const Payload bytes = payload(value);
  return core.try_publish(std::span<const std::byte>{bytes}).ok();
}

[[nodiscard]] bool processes_available_range_in_order() {
  WalCore core;
  if (!open_core(core) || !publish(core, 1) || !publish(core, 2) ||
      !publish(core, 3)) {
    return false;
  }

  WalHeadProgress upstream(core);
  Progress progress;
  RecordingModule module(progress.reader());
  Slider slider(core, upstream, progress.writer(), module);

  const SliderResult result = slider.process_available();
  if (result.status != SliderStatus::Processed || result.processed_count != 3 ||
      result.current != 3 || slider.current() != 3 ||
      progress.reader().acquire() != 3 || module.count() != 3) {
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
  WalCore core;
  if (!open_core(core) || !publish(core, 1) || !publish(core, 2) ||
      !publish(core, 3)) {
    return false;
  }

  Progress upstream(2);
  Progress own;
  RecordingModule module(own.reader(), 1);
  Slider slider(core, upstream.reader(), own.writer(), module);

  const SliderResult failed = slider.process_available();
  if (failed.status != SliderStatus::ModuleFailed ||
      failed.processed_count != 1 || failed.current != 1 ||
      slider.current() != 1 || own.reader().acquire() != 1 ||
      module.count() != 1 || module.position(0) != 0) {
    return false;
  }

  module.allow_all();
  const SliderResult retried = slider.process_available();
  if (retried.status != SliderStatus::Processed ||
      retried.processed_count != 1 || retried.current != 2 ||
      module.count() != 2 || module.position(1) != 1 ||
      own.reader().acquire() != 2) {
    return false;
  }

  if (!upstream.writer().publish(3)) return false;
  const SliderResult last = slider.process_available();
  return last.status == SliderStatus::Processed &&
         last.processed_count == 1 && last.current == 3 &&
         module.position(2) == 2 && own.reader().acquire() == 3;
}

struct InvalidRangeAcquire final {
  [[nodiscard]] PositionRange acquire(Position current,
                                      Position available) const noexcept {
    return {current, available + 1};
  }
};

struct WholeRangePublish final {
  template <class Module>
  [[nodiscard]] PublishDecision after_process(Module&,
                                              Position) const noexcept {
    return PublishDecision::Hold;
  }

  template <class Module>
  [[nodiscard]] PublishDecision after_range(Module&, Position,
                                            Position) const noexcept {
    return PublishDecision::Publish;
  }
};

[[nodiscard]] bool publishes_according_to_range_policy() {
  WalCore core;
  if (!open_core(core) || !publish(core, 1) || !publish(core, 2) ||
      !publish(core, 3)) {
    return false;
  }

  WalHeadProgress upstream(core);
  Progress progress;
  RecordingModule module(progress.reader());
  Slider slider(core, upstream, progress.writer(), module, 0,
                AvailableRangeAcquire{}, WholeRangePublish{});

  const SliderResult result = slider.process_available();
  return result.status == SliderStatus::Processed &&
         result.processed_count == 3 && progress.reader().acquire() == 3 &&
         module.published_on_entry(0) == 0 &&
         module.published_on_entry(1) == 0 &&
         module.published_on_entry(2) == 0;
}

[[nodiscard]] bool rejects_invalid_progress_and_ranges() {
  WalCore core;
  if (!open_core(core) || !publish(core, 1)) return false;

  Progress upstream;
  Progress own(1);
  RecordingModule module(own.reader());
  Slider mismatch(core, upstream.reader(), own.writer(), module);
  if (mismatch.process_available().status != SliderStatus::ProgressMismatch) {
    return false;
  }

  Progress regressed_upstream;
  Progress regressed_own(1);
  RecordingModule regressed_module(regressed_own.reader());
  Slider regressed(core, regressed_upstream.reader(), regressed_own.writer(),
                   regressed_module, 1);
  if (regressed.process_available().status !=
      SliderStatus::UpstreamRegression) {
    return false;
  }

  Progress available(1);
  Progress ranged_own;
  RecordingModule ranged_module(ranged_own.reader());
  Slider invalid(core, available.reader(), ranged_own.writer(), ranged_module,
                 0, InvalidRangeAcquire{});
  return invalid.process_available().status == SliderStatus::InvalidRange &&
         ranged_module.count() == 0 && ranged_own.reader().acquire() == 0;
}

[[nodiscard]] bool reports_unavailable_views_without_publication() {
  WalCore core;
  if (!open_core(core) || !publish(core, 1)) return false;

  Progress upstream(1);
  Progress own;
  RecordingModule module(own.reader());
  Slider reclaimed(core, upstream.reader(), own.writer(), module);
  if (core.reclaim(1) != ReclaimStatus::Ok) return false;

  const SliderResult reclaimed_result = reclaimed.process_available();
  if (reclaimed_result.status != SliderStatus::ViewUnavailable ||
      reclaimed_result.view_status != ViewStatus::Reclaimed ||
      reclaimed_result.processed_count != 0 || own.reader().acquire() != 0) {
    return false;
  }

  WalCore second;
  if (!open_core(second)) return false;
  Progress ahead(1);
  Progress second_own;
  RecordingModule second_module(second_own.reader());
  Slider unpublished(second, ahead.reader(), second_own.writer(), second_module);
  const SliderResult unpublished_result = unpublished.process_available();
  return unpublished_result.status == SliderStatus::ViewUnavailable &&
         unpublished_result.view_status == ViewStatus::Unpublished &&
         second_own.reader().acquire() == 0 && second_module.count() == 0;
}

class RejectingProgress final {
public:
  [[nodiscard]] Position acquire() const noexcept { return 0; }
  [[nodiscard]] bool publish(Position) noexcept { return false; }
};

[[nodiscard]] bool reports_publication_failure_after_processing() {
  WalCore core;
  if (!open_core(core) || !publish(core, 1)) return false;
  WalHeadProgress upstream(core);
  RejectingProgress own;
  Progress observed;
  RecordingModule module(observed.reader());
  Slider slider(core, upstream, own, module);

  const SliderResult result = slider.process_available();
  return result.status == SliderStatus::PublishFailed &&
         result.processed_count == 1 && result.current == 1 &&
         slider.current() == 1 && module.count() == 1;
}

} // namespace

int main() {
  if (!processes_available_range_in_order()) return 1;
  if (!respects_upstream_and_retries_module_failure()) return 2;
  if (!publishes_according_to_range_policy()) return 3;
  if (!rejects_invalid_progress_and_ranges()) return 4;
  if (!reports_unavailable_views_without_publication()) return 5;
  if (!reports_publication_failure_after_processing()) return 6;
  return 0;
}
