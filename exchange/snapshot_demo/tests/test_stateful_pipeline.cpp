/**
 * @file test_stateful_pipeline.cpp
 * @brief Deterministic proof of the first complete Demo 006 stateful tract.
 */

#include <fexma/snapshot_demo/bit_accumulator.hpp>
#include <fexma/snapshot_demo/hash_chain.hpp>
#include <fexma/snapshot_demo/record.hpp>
#include <fexma/wal/persistence_slider.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>

using namespace fexma;

namespace {

using Payload = snapshot_demo::ApplicationPayload;

[[nodiscard]] Payload payload(wal::Position position) noexcept {
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

[[nodiscard]] std::filesystem::path test_path(const char* name) {
  return std::filesystem::temp_directory_path() / name;
}

[[nodiscard]] bool accepted(const wal::SliderResult& result) noexcept {
  return result.status == wal::SliderStatus::Processed ||
         result.status == wal::SliderStatus::Empty;
}

template <class Module>
[[nodiscard]] bool ordering_violation_is_terminal() noexcept {
  const Payload first_payload = payload(0);
  const Payload second_payload = payload(1);

  Module repeated(10);
  if (!repeated.process({0, first_payload}) ||
      repeated.process({0, first_payload}) || !repeated.state().failed ||
      repeated.state().processed_end != 1) {
    return false;
  }

  Module skipped(10);
  if (skipped.process({1, second_payload}) || !skipped.state().failed ||
      skipped.state().processed_end != 0) {
    return false;
  }

  Module reordered(10);
  return reordered.process({0, first_payload}) &&
         !reordered.process({2, second_payload}) &&
         reordered.state().failed && reordered.state().processed_end == 1;
}

[[nodiscard]] bool payload_changes_terminal_state() noexcept {
  snapshot_demo::HashChainModule first_hash(100);
  snapshot_demo::HashChainModule second_hash(100);
  snapshot_demo::BitAccumulatorModule first_bits(100);
  snapshot_demo::BitAccumulatorModule second_bits(100);

  for (wal::Position position = 0; position < 4; ++position) {
    Payload first = payload(position);
    Payload second = first;
    if (position == 2) second[23] ^= std::byte{0x40};
    const wal::RecordView first_record{position, first};
    const wal::RecordView second_record{position, second};
    if (!first_hash.process(first_record) || !second_hash.process(second_record) ||
        !first_bits.process(first_record) || !second_bits.process(second_record)) {
      return false;
    }
  }

  return first_hash.state().digest != second_hash.state().digest &&
         (first_bits.state().total_one_bits !=
              second_bits.state().total_one_bits ||
          first_bits.state().rolling_bits != second_bits.state().rolling_bits);
}

[[nodiscard]] bool complete_stateful_tract_preserves_invariants() {
  constexpr wal::Position message_count = 2048;
  constexpr std::uint32_t capacity = 64;
  constexpr wal::Position first_sequence = 100;
  const auto path = test_path("fexma_snapshot_demo_stateful_pipeline.wal");
  std::filesystem::remove(path);

  wal::RecordTape source;
  wal::PersistenceModule persistence;
  if (!source.open({static_cast<std::uint32_t>(sizeof(Payload)), capacity,
                    wal::default_alignment})
           .ok() ||
      !persistence
           .open(path, {static_cast<std::uint32_t>(sizeof(Payload)),
                        wal::wal_default_alignment, 1, wal::StreamKind::Generic,
                        31, 7, first_sequence, 13})
           .ok()) {
    return false;
  }

  wal::Frontier durable;
  wal::PersistenceSlider persistence_slider(source, durable, persistence, 1);

  snapshot_demo::HashChainModule hash_module(first_sequence);
  wal::Frontier hash_frontier;
  wal::Slider hash_slider(source, durable, hash_frontier,
                          hash_module);

  snapshot_demo::BitAccumulatorModule bit_module(first_sequence);
  wal::Frontier bit_frontier;
  wal::Slider bit_slider(source, hash_frontier, bit_frontier,
                         bit_module);

  snapshot_demo::HashChainModule expected_hash(first_sequence);
  snapshot_demo::BitAccumulatorModule expected_bits(first_sequence);
  wal::Position produced = 0;
  std::uint64_t cycle = 0;
  bool saw_full = false;
  bool saw_persistence_lead = false;
  bool saw_hash_lead = false;

  while (source.tail() != message_count) {
    while (produced < message_count) {
      const Payload bytes = payload(produced);
      const wal::PublishResult result =
          source.try_publish(std::span<const std::byte>{bytes});
      if (result.status == wal::PublishStatus::Full) {
        saw_full = true;
        break;
      }
      if (!result.ok() || result.position != produced) {
        return false;
      }
      const wal::RecordView expected_record{produced, bytes};
      if (!expected_hash.process(expected_record) ||
          !expected_bits.process(expected_record)) {
        return false;
      }
      ++produced;
    }

    persistence_slider.set_maximum_count(1 + cycle % 17);
    if (!accepted(persistence_slider.process_available())) return false;
    if ((cycle % 3) == 0 && !accepted(hash_slider.process_available())) {
      return false;
    }
    if ((cycle % 7) == 0) {
      if (!accepted(bit_slider.process_available()) ||
          source.reclaim(bit_frontier.acquire()) !=
              wal::ReclaimStatus::Ok) {
        return false;
      }
    }

    const wal::Position tail = source.tail();
    const wal::Position bit = bit_frontier.acquire();
    const wal::Position hash = hash_frontier.acquire();
    const wal::Position durable_end = durable.acquire();
    const wal::Position head_end = source.head();
    if (!(tail <= bit && bit <= hash && hash <= durable_end &&
          durable_end <= head_end)) {
      return false;
    }
    saw_persistence_lead = saw_persistence_lead || durable_end > hash;
    saw_hash_lead = saw_hash_lead || hash > bit;
    if (++cycle > message_count * 20) return false;
  }

  const bool valid = produced == message_count && saw_full &&
                     saw_persistence_lead && saw_hash_lead &&
                     source.tail() == bit_frontier.acquire() &&
                     source.tail() == hash_frontier.acquire() &&
                     source.tail() == durable.acquire() &&
                     source.tail() == source.head() && !hash_module.state().failed &&
                     !bit_module.state().failed &&
                     hash_module.state() == expected_hash.state() &&
                     bit_module.state() == expected_bits.state();
  const bool persistence_closed = persistence.close();
  source.close();
  std::filesystem::remove(path);
  return valid && persistence_closed;
}

} // namespace

int main() {
  if (!payload_changes_terminal_state()) return 1;
  if (!ordering_violation_is_terminal<snapshot_demo::HashChainModule>()) {
    return 2;
  }
  if (!ordering_violation_is_terminal<snapshot_demo::BitAccumulatorModule>()) {
    return 3;
  }
  if (!complete_stateful_tract_preserves_invariants()) return 4;
  return 0;
}
