/**
 * @file test_wal_bare_pipeline.cpp
 * @brief Sequential and concurrent proof of the bare WAL slider pipeline.
 */

#include <fexma/wal/noop_module.hpp>
#include <fexma/wal/slider.hpp>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <thread>

using namespace fexma::wal;

namespace {

using Payload = std::array<std::byte, 16>;

[[nodiscard]] Payload payload(Position position) noexcept {
  Payload result{};
  const std::uint64_t inverse = ~position;
  for (std::size_t index = 0; index < 8; ++index) {
    result[index] =
        static_cast<std::byte>((position >> (index * 8u)) & 0xffu);
    result[index + 8] =
        static_cast<std::byte>((inverse >> (index * 8u)) & 0xffu);
  }
  return result;
}

[[nodiscard]] bool equal(std::span<const std::byte> actual,
                         const Payload& expected) noexcept {
  if (actual.size() != expected.size()) return false;
  for (std::size_t index = 0; index < expected.size(); ++index) {
    if (actual[index] != expected[index]) return false;
  }
  return true;
}

[[nodiscard]] bool publish(WalCore& wal, Position position) noexcept {
  const Payload bytes = payload(position);
  const PublishResult result =
      wal.try_publish(std::span<const std::byte>{bytes});
  return result.ok() && result.sequence == position + 1;
}

static_assert(SliderModule<NoOpModule>);

[[nodiscard]] bool stopped_slider_preserves_backpressure() {
  constexpr Position capacity = 3;
  WalCore wal;
  if (!wal.open({static_cast<std::uint32_t>(sizeof(Payload)),
                 static_cast<std::uint32_t>(capacity), default_alignment, 1})
           .ok()) {
    return false;
  }

  for (Position position = 0; position < capacity; ++position) {
    if (!publish(wal, position)) return false;
  }
  const Payload blocked_payload = payload(capacity);
  if (wal.try_publish(std::span<const std::byte>{blocked_payload}).status !=
          PublishStatus::Full ||
      wal.tail() != 0 || wal.head() != capacity) {
    return false;
  }

  WalHeadProgress head(wal);
  Progress no_op_frontier;
  NoOpModule module;
  Slider slider(wal, head, no_op_frontier.writer(), module);

  const SliderResult processed = slider.process_available();
  if (processed.status != SliderStatus::Processed ||
      processed.processed_count != capacity ||
      no_op_frontier.reader().acquire() != capacity || wal.tail() != 0 ||
      wal.try_publish(std::span<const std::byte>{blocked_payload}).status !=
          PublishStatus::Full) {
    return false;
  }

  if (wal.reclaim(no_op_frontier.reader().acquire()) != ReclaimStatus::Ok ||
      !publish(wal, capacity)) {
    return false;
  }
  const SliderResult wrapped = slider.process_available();
  if (!wrapped.ok() || wrapped.current != capacity + 1 ||
      wal.reclaim(no_op_frontier.reader().acquire()) != ReclaimStatus::Ok) {
    return false;
  }

  return wal.tail() == capacity + 1 &&
         no_op_frontier.reader().acquire() == wal.tail() &&
         wal.head() == wal.tail();
}

[[nodiscard]] bool retains_slots_until_composition_reclaims() {
  WalCore wal;
  if (!wal.open({static_cast<std::uint32_t>(sizeof(Payload)), 2,
                 default_alignment, 1})
           .ok() ||
      !publish(wal, 0) || !publish(wal, 1)) {
    return false;
  }

  const AccessResult before = wal.try_view(0);
  if (!before.ok() || !equal(before.record.payload, payload(0))) return false;
  const std::byte* const first_address = before.record.payload.data();

  WalHeadProgress head(wal);
  Progress no_op_frontier;
  NoOpModule module;
  Slider slider(wal, head, no_op_frontier.writer(), module);
  if (!slider.process_available().ok()) return false;

  const AccessResult retained = wal.try_view(0);
  const Payload third = payload(2);
  if (!retained.ok() || retained.record.payload.data() != first_address ||
      !equal(retained.record.payload, payload(0)) || wal.tail() != 0 ||
      wal.try_publish(std::span<const std::byte>{third}).status !=
          PublishStatus::Full) {
    return false;
  }

  // No borrowed view of position 0 is used after this reclamation.
  if (wal.reclaim(1) != ReclaimStatus::Ok || !publish(wal, 2) ||
      wal.try_view(0).status != ViewStatus::Reclaimed) {
    return false;
  }
  const AccessResult replacement = wal.try_view(2);
  if (!replacement.ok() || replacement.record.payload.data() != first_address ||
      !equal(replacement.record.payload, payload(2))) {
    return false;
  }

  if (!slider.process_available().ok() ||
      no_op_frontier.reader().acquire() != 3 ||
      wal.reclaim(no_op_frontier.reader().acquire()) != ReclaimStatus::Ok) {
    return false;
  }
  return wal.tail() == 3 && wal.head() == 3;
}

class OrderedProbeModule final {
public:
  explicit OrderedProbeModule(Position first_sequence) noexcept
      : first_sequence_(first_sequence) {}

  [[nodiscard]] bool process(const RecordView& record) noexcept {
    const Payload expected_payload = payload(expected_position_);
    if (record.position != expected_position_ ||
        record.sequence != first_sequence_ + expected_position_ ||
        !equal(record.payload, expected_payload)) {
      valid_ = false;
      return false;
    }
    ++expected_position_;
    return true;
  }

  [[nodiscard]] bool valid() const noexcept { return valid_; }
  [[nodiscard]] Position count() const noexcept { return expected_position_; }

private:
  Position first_sequence_{};
  Position expected_position_{};
  bool valid_{true};
};

static_assert(SliderModule<OrderedProbeModule>);

[[nodiscard]] bool producer_and_slider_wrap_concurrently() {
  constexpr Position message_count = 200'000;
  constexpr std::uint32_t capacity = 127;
  constexpr Position first_sequence = 1000;

  WalCore wal;
  if (!wal.open({static_cast<std::uint32_t>(sizeof(Payload)), capacity,
                 default_alignment, first_sequence})
           .ok()) {
    return false;
  }

  WalHeadProgress head(wal);
  Progress no_op_frontier;
  OrderedProbeModule module(first_sequence);
  Slider slider(wal, head, no_op_frontier.writer(), module);
  std::atomic<bool> failed{false};
  std::atomic<bool> producer_done{false};

  std::thread producer([&] {
    for (Position position = 0; position < message_count;) {
      if (failed.load(std::memory_order_acquire)) return;
      const Payload bytes = payload(position);
      const PublishResult result =
          wal.try_publish(std::span<const std::byte>{bytes});
      if (result.ok()) {
        if (result.sequence != first_sequence + position) {
          failed.store(true, std::memory_order_release);
          return;
        }
        ++position;
      } else if (result.status == PublishStatus::Full) {
        std::this_thread::yield();
      } else {
        failed.store(true, std::memory_order_release);
        return;
      }
    }
    producer_done.store(true, std::memory_order_release);
  });

  std::thread consumer([&] {
    while (slider.current() < message_count) {
      if (failed.load(std::memory_order_acquire)) return;
      const SliderResult result = slider.process_available();
      if (result.status == SliderStatus::Processed) {
        const Position published = no_op_frontier.reader().acquire();
        if (published != slider.current() || published > wal.head() ||
            wal.reclaim(published) != ReclaimStatus::Ok) {
          failed.store(true, std::memory_order_release);
          return;
        }
      } else if (result.status == SliderStatus::Empty) {
        if (producer_done.load(std::memory_order_acquire) &&
            slider.current() != message_count) {
          failed.store(true, std::memory_order_release);
          return;
        }
        std::this_thread::yield();
      } else {
        failed.store(true, std::memory_order_release);
        return;
      }
    }
  });

  producer.join();
  consumer.join();

  return !failed.load(std::memory_order_acquire) && module.valid() &&
         module.count() == message_count && wal.tail() == message_count &&
         no_op_frontier.reader().acquire() == message_count &&
         wal.head() == message_count;
}

} // namespace

int main() {
  if (!stopped_slider_preserves_backpressure()) return 1;
  if (!retains_slots_until_composition_reclaims()) return 2;
  if (!producer_and_slider_wrap_concurrently()) return 3;
  return 0;
}
