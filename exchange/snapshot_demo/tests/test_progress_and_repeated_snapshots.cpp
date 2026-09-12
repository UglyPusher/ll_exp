/**
 * @file test_progress_and_repeated_snapshots.cpp
 * @brief Frontier-violation and repeated-generation checks for Demo 006.
 */

#include <fexma/snapshot_demo/record.hpp>
#include <fexma/snapshot_demo/snapshot_loader.hpp>
#include <fexma/snapshot_demo/snapshot_sink.hpp>
#include <fexma/wal/persistence_slider.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

using namespace fexma;

namespace {

using Payload = snapshot_demo::ApplicationPayload;

constexpr std::uint64_t first_sequence = 5000;
constexpr snapshot_demo::SnapshotIdentity identity{
    wal::StreamKind::Generic, 31, 7, 13, 6006};

struct ExpectedSnapshot {
  wal::Position position{};
  snapshot_demo::HashChainState hash{};
  snapshot_demo::BitAccumulatorState bits{};
};

[[nodiscard]] Payload data_payload(wal::Position position) noexcept {
  snapshot_demo::ApplicationData data{};
  std::uint64_t value = position + 0x9e3779b97f4a7c15ull;
  for (std::size_t index = 0; index < data.size(); ++index) {
    value ^= value >> 12u;
    value ^= value << 25u;
    value ^= value >> 27u;
    data[index] = static_cast<std::byte>(value & 0xffu);
  }
  return snapshot_demo::encode_data(data);
}

[[nodiscard]] bool accepted(const wal::SliderResult& result) noexcept {
  return result.status == wal::SliderStatus::Processed ||
         result.status == wal::SliderStatus::Empty;
}

[[nodiscard]] bool retryable_snapshot_backpressure(
    const wal::SliderResult& result,
    const snapshot_demo::HashChainModule& module) noexcept {
  return result.status == wal::SliderStatus::ModuleFailed &&
         !module.state().failed && module.pending_capture() != nullptr;
}

[[nodiscard]] bool retryable_snapshot_backpressure(
    const wal::SliderResult& result,
    const snapshot_demo::BitAccumulatorModule& module) noexcept {
  return result.status == wal::SliderStatus::ModuleFailed &&
         !module.state().failed && module.pending_capture() != nullptr;
}

[[nodiscard]] bool downstream_ahead_of_upstream_is_rejected() noexcept {
  wal::RecordTape source;
  if (!source.open({static_cast<std::uint32_t>(sizeof(Payload)), 2,
                    wal::default_alignment})
           .ok()) {
    return false;
  }

  wal::Frontier upstream(1);
  wal::Frontier downstream(2);
  snapshot_demo::BitAccumulatorModule bits(first_sequence);
  snapshot_demo::BitAccumulatorState restored{};
  restored.processed_end = 2;
  bits.restore_quiescent(restored);
  wal::Slider slider(source, upstream, downstream, bits);

  const wal::SliderResult result = slider.process_available();
  const bool valid = result.status == wal::SliderStatus::UpstreamRegression &&
                     result.current == 2 && result.processed_count == 0 &&
                     slider.current() == 2 &&
                     downstream.acquire() == 2 &&
                     bits.state() == restored;
  source.close();
  return valid;
}

[[nodiscard]] bool premature_tail_breaks_last_mandatory_stage() noexcept {
  wal::RecordTape source;
  if (!source.open({static_cast<std::uint32_t>(sizeof(Payload)), 4,
                    wal::default_alignment})
           .ok()) {
    return false;
  }
  for (wal::Position position = 0; position < 3; ++position) {
    const Payload payload = data_payload(position);
    if (!source.try_publish(std::span<const std::byte>{payload}).ok()) {
      source.close();
      return false;
    }
  }

  wal::Frontier bit_upstream(1);
  wal::Frontier bit_frontier;
  snapshot_demo::BitAccumulatorModule bits(first_sequence);
  wal::Slider bit_slider(source, bit_upstream, bit_frontier, bits);

  if (!accepted(bit_slider.process_available()) || bit_frontier.acquire() != 1 ||
      source.reclaim(bit_frontier.acquire()) !=
          wal::ReclaimStatus::Ok ||
      !bit_upstream.publish(2) ||
      !accepted(bit_slider.process_available()) ||
      bit_frontier.acquire() != 2) {
    source.close();
    return false;
  }

  const snapshot_demo::BitAccumulatorState retained_state = bits.state();
  if (source.reclaim(3) != wal::ReclaimStatus::Ok ||
      !bit_upstream.publish(3)) {
    source.close();
    return false;
  }
  const wal::SliderResult result = bit_slider.process_available();
  const bool valid = source.tail() == 3 &&
                     result.status == wal::SliderStatus::ViewUnavailable &&
                     result.view_status == wal::ViewStatus::Reclaimed &&
                     result.current == 2 && result.processed_count == 0 &&
                     bit_slider.current() == 2 &&
                     bit_frontier.acquire() == 2 &&
                     bits.state() == retained_state;
  source.close();
  return valid;
}

[[nodiscard]] std::uint64_t next_random(std::uint64_t& state) noexcept {
  state ^= state >> 12u;
  state ^= state << 25u;
  state ^= state >> 27u;
  return state * 2685821657736338717ull;
}

[[nodiscard]] std::vector<wal::Position>
random_snapshot_positions(wal::Position record_count) {
  std::vector<wal::Position> positions{0};
  std::uint64_t random_state = 0x6a09e667f3bcc909ull;
  for (wal::Position position = 1; position + 1 < record_count; ++position) {
    if (next_random(random_state) % 11u == 0) positions.push_back(position);
  }
  positions.push_back(record_count - 1u);
  return positions;
}

[[nodiscard]] bool run_repeated_snapshot_case(
    const char* name, wal::Position record_count,
    const std::vector<wal::Position>& snapshot_positions) {
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / name;
  const std::filesystem::path wal_path = root.string() + ".wal";
  std::filesystem::remove_all(root);
  std::filesystem::remove(wal_path);

  wal::RecordTape source;
  wal::PersistenceModule persistence;
  if (!source.open({static_cast<std::uint32_t>(sizeof(Payload)), 32,
                    wal::default_alignment})
           .ok() ||
      !persistence
           .open(wal_path,
                 {static_cast<std::uint32_t>(sizeof(Payload)),
                  wal::wal_default_alignment, 1, identity.stream_kind,
                  identity.stream_id, identity.epoch_id, first_sequence,
                  identity.manifest_id})
           .ok()) {
    source.close();
    std::filesystem::remove_all(root);
    std::filesystem::remove(wal_path);
    return false;
  }

  wal::Frontier durable;
  wal::Frontier hash_frontier;
  wal::Frontier bit_frontier;
  wal::PersistenceSlider persistence_slider(source, durable, persistence, 1);
  snapshot_demo::HashChainModule hash(first_sequence);
  snapshot_demo::BitAccumulatorModule bits(first_sequence);
  wal::Slider hash_slider(source, durable, hash_frontier, hash);
  wal::Slider bit_slider(source, hash_frontier, bit_frontier,
                         bits);
  snapshot_demo::CaptureGenerationCoordinator coordinator;
  const snapshot_demo::SnapshotSink sink(root, identity);

  snapshot_demo::HashChainModule expected_hash(first_sequence);
  snapshot_demo::BitAccumulatorModule expected_bits(first_sequence);
  std::vector<ExpectedSnapshot> expected_snapshots;
  expected_snapshots.reserve(snapshot_positions.size());

  wal::Position produced = 0;
  std::size_t next_snapshot = 0;
  std::size_t saved_count = 0;
  std::uint64_t schedule_state = 0xbb67ae8584caa73bull;
  std::uint64_t cycle = 0;
  bool valid = true;

  while (valid && source.tail() != record_count) {
    while (produced < record_count) {
      const bool is_snapshot =
          next_snapshot < snapshot_positions.size() &&
          snapshot_positions[next_snapshot] == produced;
      const Payload payload = is_snapshot
                                  ? snapshot_demo::encode_save_snapshot(produced)
                                  : data_payload(produced);
      const wal::PublishResult published =
          source.try_publish(std::span<const std::byte>{payload});
      if (published.status == wal::PublishStatus::Full) break;
      if (!published.ok() || published.position != produced ||
          !expected_hash.process(
              {produced, std::span<const std::byte>{payload}}) ||
          !expected_bits.process(
              {produced, std::span<const std::byte>{payload}})) {
        valid = false;
        break;
      }
      if (is_snapshot) {
        expected_snapshots.push_back(
            {produced, expected_hash.state(), expected_bits.state()});
        if (!expected_hash.release_capture(produced, produced + 1u) ||
            !expected_bits.release_capture(produced, produced + 1u)) {
          valid = false;
          break;
        }
        ++next_snapshot;
      }
      ++produced;
    }
    if (!valid) break;

    persistence_slider.set_maximum_count(
        1u + next_random(schedule_state) % 13u);

    const wal::SliderResult persistence_result =
        persistence_slider.process_available();
    const wal::SliderResult hash_result = hash_slider.process_available();
    const wal::SliderResult bit_result = bit_slider.process_available();
    if (!accepted(persistence_result) ||
        (!accepted(hash_result) &&
         !retryable_snapshot_backpressure(hash_result, hash)) ||
        (!accepted(bit_result) &&
         !retryable_snapshot_backpressure(bit_result, bits))) {
      valid = false;
      break;
    }

    if (hash.pending_capture() != nullptr &&
        bits.pending_capture() != nullptr) {
      if (coordinator.collect(hash, bits) !=
              snapshot_demo::CollectStatus::Complete ||
          !sink.save_and_release(coordinator, hash, bits).ok()) {
        valid = false;
        break;
      }
      ++saved_count;
    }

    const wal::Position tail = source.tail();
    const wal::Position bit = bit_frontier.acquire();
    const wal::Position hash_end = hash_frontier.acquire();
    const wal::Position durable_end = durable.acquire();
    const wal::Position head_end = source.head();
    if (!(tail <= bit && bit <= hash_end && hash_end <= durable_end &&
          durable_end <= head_end) ||
        source.reclaim(bit) != wal::ReclaimStatus::Ok ||
        ++cycle > record_count * 50u) {
      valid = false;
    }
  }

  valid = valid && produced == record_count &&
          next_snapshot == snapshot_positions.size() &&
          saved_count == snapshot_positions.size() &&
          expected_snapshots.size() == snapshot_positions.size() &&
          source.tail() == record_count &&
          bit_frontier.acquire() == record_count &&
          hash_frontier.acquire() == record_count &&
          durable.acquire() == record_count &&
          source.head() == record_count && !hash.state().failed &&
          !bits.state().failed && hash.state() == expected_hash.state() &&
          bits.state() == expected_bits.state() &&
          coordinator.pending_generation() == nullptr &&
          hash.pending_capture() == nullptr && bits.pending_capture() == nullptr;

  const snapshot_demo::SnapshotLoader loader(root, identity);
  for (const ExpectedSnapshot& expected : expected_snapshots) {
    const snapshot_demo::SnapshotLoadResult loaded =
        loader.load(expected.position);
    valid = valid && loaded.ok() &&
            loaded.prepared->description.generation_id == expected.position &&
            loaded.prepared->description.record_position == expected.position &&
            loaded.prepared->description.processed_end ==
                expected.position + 1u &&
            loaded.prepared->hash_chain == expected.hash &&
            loaded.prepared->bit_accumulator == expected.bits;
  }

  const bool persistence_closed = persistence.close();
  source.close();
  std::filesystem::remove_all(root);
  std::filesystem::remove(wal_path);
  return valid && persistence_closed;
}

} // namespace

int main() {
  if (!downstream_ahead_of_upstream_is_rejected()) return 1;
  if (!premature_tail_breaks_last_mandatory_stage()) return 2;

  const std::vector<wal::Position> deterministic{
      0, 1, 2, 7, 15, 16, 31, 63, 95, 127};
  if (!run_repeated_snapshot_case(
          "fexma_snapshot_demo_repeated_deterministic", 128,
          deterministic)) {
    return 3;
  }

  constexpr wal::Position randomized_record_count = 257;
  const std::vector<wal::Position> randomized =
      random_snapshot_positions(randomized_record_count);
  if (randomized.size() < 10 ||
      !run_repeated_snapshot_case("fexma_snapshot_demo_repeated_randomized",
                                  randomized_record_count, randomized)) {
    return 4;
  }
  return 0;
}
