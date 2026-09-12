/**
 * @file test_snapshot_semantics.cpp
 * @brief Codec, capture, generation, and backpressure checks for Demo 006.
 */

#include <fexma/snapshot_demo/capture_generation.hpp>
#include <fexma/snapshot_demo/record.hpp>
#include <fexma/wal/slider.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

using namespace fexma;

namespace {

[[nodiscard]] snapshot_demo::ApplicationData data(std::uint8_t seed) noexcept {
  snapshot_demo::ApplicationData result{};
  for (std::size_t index = 0; index < result.size(); ++index) {
    result[index] = static_cast<std::byte>(seed + index);
  }
  return result;
}

[[nodiscard]] bool codec_is_fixed_and_strict() noexcept {
  const auto source_data = data(9);
  const auto encoded_data = snapshot_demo::encode_data(source_data);
  const auto decoded_data = snapshot_demo::decode_record(encoded_data);
  if (!decoded_data ||
      decoded_data.record.kind != snapshot_demo::RecordKind::Data ||
      decoded_data.record.generation_id != 0 ||
      !std::equal(decoded_data.record.data.begin(),
                  decoded_data.record.data.end(), source_data.begin())) {
    return false;
  }

  const auto encoded_snapshot = snapshot_demo::encode_save_snapshot(27);
  const auto decoded_snapshot = snapshot_demo::decode_record(encoded_snapshot);
  if (!decoded_snapshot ||
      decoded_snapshot.record.kind != snapshot_demo::RecordKind::SaveSnapshot ||
      decoded_snapshot.record.generation_id != 27) {
    return false;
  }

  auto invalid_magic = encoded_data;
  invalid_magic[0] ^= std::byte{1};
  auto invalid_version = encoded_data;
  invalid_version[4] ^= std::byte{1};
  auto invalid_kind = encoded_data;
  invalid_kind[6] = std::byte{3};
  auto invalid_data_generation = encoded_data;
  invalid_data_generation[8] = std::byte{1};
  auto invalid_snapshot_data = encoded_snapshot;
  invalid_snapshot_data[16] = std::byte{1};
  const std::array<std::byte, 1> short_payload{};

  return snapshot_demo::decode_record(short_payload).status ==
             snapshot_demo::DecodeStatus::InvalidSize &&
         snapshot_demo::decode_record(invalid_magic).status ==
             snapshot_demo::DecodeStatus::InvalidMagic &&
         snapshot_demo::decode_record(invalid_version).status ==
             snapshot_demo::DecodeStatus::UnsupportedVersion &&
         snapshot_demo::decode_record(invalid_kind).status ==
             snapshot_demo::DecodeStatus::InvalidKind &&
         snapshot_demo::decode_record(invalid_data_generation).status ==
             snapshot_demo::DecodeStatus::InvalidGeneration &&
         snapshot_demo::decode_record(invalid_snapshot_data).status ==
             snapshot_demo::DecodeStatus::NonZeroSnapshotData;
}

template <class Module>
[[nodiscard]] bool malformed_record_is_terminal() noexcept {
  auto malformed = snapshot_demo::encode_data(data(1));
  malformed[0] ^= std::byte{1};
  Module module(10);
  return !module.process({0, malformed}) && module.state().failed &&
         module.state().processed_end == 0 &&
         module.pending_capture() == nullptr;
}

template <class Module>
[[nodiscard]] bool snapshot_generation_is_its_position() noexcept {
  const auto wrong_generation = snapshot_demo::encode_save_snapshot(9);
  Module module(10);
  return !module.process({0, wrong_generation}) && module.state().failed &&
         module.state().processed_end == 0 &&
         module.pending_capture() == nullptr;
}

template <class Module, class Capture>
[[nodiscard]] bool capture_is_state_after_and_immutable() noexcept {
  const auto snapshot = snapshot_demo::encode_save_snapshot(0);
  const auto ordinary = snapshot_demo::encode_data(data(5));
  Module module(100);
  if (!module.process({0, snapshot})) return false;
  const Capture* pending = module.pending_capture();
  if (pending == nullptr || pending->generation_id != 0 ||
      pending->record_position != 0 || pending->processed_end != 1 ||
      pending->sequence != 100 || pending->state != module.state()) {
    return false;
  }
  const Capture captured = *pending;
  if (!module.process({1, ordinary}) || module.state().processed_end != 2) {
    return false;
  }
  return module.pending_capture() != nullptr &&
         *module.pending_capture() == captured;
}

[[nodiscard]] bool coordinator_waits_and_checks_identity() noexcept {
  const auto snapshot = snapshot_demo::encode_save_snapshot(0);
  snapshot_demo::HashChainModule hash(70);
  snapshot_demo::BitAccumulatorModule bits(70);
  snapshot_demo::CaptureGenerationCoordinator coordinator;

  if (!hash.process({0, snapshot}) ||
      coordinator.collect(hash, bits) !=
          snapshot_demo::CollectStatus::MissingParticipant ||
      coordinator.pending_generation() != nullptr ||
      hash.pending_capture() == nullptr) {
    return false;
  }
  if (!bits.process({0, snapshot}) ||
      coordinator.collect(hash, bits) !=
          snapshot_demo::CollectStatus::Complete) {
    return false;
  }
  const auto* generation = coordinator.pending_generation();
  if (generation == nullptr || generation->generation_id != 0 ||
      generation->record_position != 0 || generation->processed_end != 1 ||
      generation->sequence != 70 ||
      generation->hash_chain != *hash.pending_capture() ||
      generation->bit_accumulator != *bits.pending_capture() ||
      coordinator.collect(hash, bits) !=
          snapshot_demo::CollectStatus::GenerationInFlight ||
      !coordinator.release_generation(hash, bits, 0)) {
    return false;
  }
  if (coordinator.pending_generation() != nullptr ||
      hash.pending_capture() != nullptr || bits.pending_capture() != nullptr) {
    return false;
  }

  snapshot_demo::HashChainModule mismatched_hash(80);
  snapshot_demo::BitAccumulatorModule mismatched_bits(81);
  snapshot_demo::CaptureGenerationCoordinator mismatched_coordinator;
  return mismatched_hash.process({0, snapshot}) &&
         mismatched_bits.process({0, snapshot}) &&
         mismatched_coordinator.collect(mismatched_hash, mismatched_bits) ==
             snapshot_demo::CollectStatus::Mismatch &&
         mismatched_coordinator.pending_generation() == nullptr;
}

[[nodiscard]] bool one_generation_backpressures_next_snapshot() noexcept {
  wal::RecordTape source;
  if (!source.open({static_cast<std::uint32_t>(
                        snapshot_demo::kApplicationPayloadSize),
                    8, wal::default_alignment})
           .ok()) {
    return false;
  }

  const auto first_snapshot = snapshot_demo::encode_save_snapshot(0);
  const auto ordinary = snapshot_demo::encode_data(data(12));
  const auto second_snapshot = snapshot_demo::encode_save_snapshot(2);
  if (!source.try_publish(first_snapshot).ok() ||
      !source.try_publish(ordinary).ok() ||
      !source.try_publish(second_snapshot).ok()) {
    return false;
  }

  wal::Frontier hash_frontier;
  wal::Frontier bit_frontier;
  snapshot_demo::HashChainModule hash(50);
  snapshot_demo::BitAccumulatorModule bits(50);
  wal::Slider hash_slider(source, hash_frontier, hash);
  wal::Slider bit_slider(source, bit_frontier, bits);

  const wal::SliderResult hash_blocked = hash_slider.process_available();
  const wal::SliderResult bits_blocked = bit_slider.process_available();
  if (hash_blocked.status != wal::SliderStatus::ModuleFailed ||
      bits_blocked.status != wal::SliderStatus::ModuleFailed ||
      hash_blocked.processed_count != 2 || bits_blocked.processed_count != 2 ||
      hash_slider.current() != 2 || bit_slider.current() != 2 ||
      hash_frontier.acquire() != 2 ||
      bit_frontier.acquire() != 2 || hash.state().failed ||
      bits.state().failed || hash.state().processed_end != 2 ||
      bits.state().processed_end != 2) {
    return false;
  }
  const snapshot_demo::HashChainState blocked_hash_state = hash.state();
  const snapshot_demo::BitAccumulatorState blocked_bits_state = bits.state();

  snapshot_demo::CaptureGenerationCoordinator coordinator;
  if (coordinator.collect(hash, bits) !=
          snapshot_demo::CollectStatus::Complete ||
      hash_slider.process_available().status !=
          wal::SliderStatus::ModuleFailed ||
      bit_slider.process_available().status !=
          wal::SliderStatus::ModuleFailed ||
      hash.state() != blocked_hash_state || bits.state() != blocked_bits_state ||
      !coordinator.release_generation(hash, bits, 0)) {
    return false;
  }

  const wal::SliderResult hash_second = hash_slider.process_available();
  const wal::SliderResult bits_second = bit_slider.process_available();
  const bool valid = hash_second.status == wal::SliderStatus::Processed &&
                     bits_second.status == wal::SliderStatus::Processed &&
                     hash_second.processed_count == 1 &&
                     bits_second.processed_count == 1 &&
                     hash_frontier.acquire() == 3 &&
                     bit_frontier.acquire() == 3 &&
                     hash.pending_capture() != nullptr &&
                     bits.pending_capture() != nullptr &&
                     hash.pending_capture()->generation_id == 2 &&
                     bits.pending_capture()->generation_id == 2;
  source.close();
  return valid;
}

} // namespace

int main() {
  if (!codec_is_fixed_and_strict()) return 1;
  if (!malformed_record_is_terminal<snapshot_demo::HashChainModule>()) return 2;
  if (!malformed_record_is_terminal<snapshot_demo::BitAccumulatorModule>()) {
    return 3;
  }
  if (!snapshot_generation_is_its_position<
          snapshot_demo::HashChainModule>()) {
    return 4;
  }
  if (!snapshot_generation_is_its_position<
          snapshot_demo::BitAccumulatorModule>()) {
    return 5;
  }
  if (!capture_is_state_after_and_immutable<snapshot_demo::HashChainModule,
                                            snapshot_demo::HashChainCapture>()) {
    return 6;
  }
  if (!capture_is_state_after_and_immutable<
          snapshot_demo::BitAccumulatorModule,
          snapshot_demo::BitAccumulatorCapture>()) {
    return 7;
  }
  if (!coordinator_waits_and_checks_identity()) return 8;
  if (!one_generation_backpressures_next_snapshot()) return 9;
  return 0;
}
