#pragma once

/**
 * @file capture_generation.hpp
 * @brief Composition-owned assembly of a complete Demo 006 generation.
 */

#include <fexma/snapshot_demo/bit_accumulator.hpp>
#include <fexma/snapshot_demo/hash_chain.hpp>
#include <fexma/wal/record_tape_types.hpp>

#include <chrono>
#include <cstdint>
#include <optional>

namespace fexma::snapshot_demo {

struct CaptureGeneration {
  std::uint64_t generation_id{};
  wal::Position record_position{};
  wal::Position processed_end{};
  std::uint64_t sequence{};
  std::uint64_t hash_capture_duration_ns{};
  std::uint64_t bit_accumulator_capture_duration_ns{};
  std::uint64_t generation_completion_duration_ns{};
  HashChainCapture hash_chain{};
  BitAccumulatorCapture bit_accumulator{};

  friend bool operator==(const CaptureGeneration&,
                         const CaptureGeneration&) = default;
};

enum class CollectStatus {
  Complete,
  MissingParticipant,
  Mismatch,
  GenerationInFlight,
};

class CaptureGenerationCoordinator final {
public:
  [[nodiscard]] CollectStatus collect(const HashChainModule& hash_chain,
                                      const BitAccumulatorModule&
                                          bit_accumulator) noexcept {
    if (generation_) return CollectStatus::GenerationInFlight;

    const auto completion_started = std::chrono::steady_clock::now();

    const HashChainCapture* hash = hash_chain.pending_capture();
    const BitAccumulatorCapture* bits = bit_accumulator.pending_capture();
    if (hash == nullptr || bits == nullptr) {
      return CollectStatus::MissingParticipant;
    }
    if (hash->generation_id != bits->generation_id ||
        hash->record_position != bits->record_position ||
        hash->processed_end != bits->processed_end ||
        hash->sequence != bits->sequence ||
        hash->state.processed_end != hash->processed_end ||
        bits->state.processed_end != bits->processed_end) {
      return CollectStatus::Mismatch;
    }

    generation_.emplace(CaptureGeneration{
        hash->generation_id, hash->record_position, hash->processed_end,
        hash->sequence, hash_chain.capture_duration_ns(),
        bit_accumulator.capture_duration_ns(), 0, *hash, *bits});
    generation_->generation_completion_duration_ns =
        static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - completion_started)
                .count());
    return CollectStatus::Complete;
  }

  [[nodiscard]] const CaptureGeneration* pending_generation() const noexcept {
    return generation_ ? &*generation_ : nullptr;
  }

  [[nodiscard]] bool release_generation(HashChainModule& hash_chain,
                                        BitAccumulatorModule& bit_accumulator,
                                        std::uint64_t generation_id) noexcept {
    if (!generation_ || generation_->generation_id != generation_id) {
      return false;
    }
    const wal::Position processed_end = generation_->processed_end;
    if (hash_chain.pending_capture() == nullptr ||
        bit_accumulator.pending_capture() == nullptr ||
        hash_chain.pending_capture()->generation_id != generation_id ||
        bit_accumulator.pending_capture()->generation_id != generation_id ||
        hash_chain.pending_capture()->processed_end != processed_end ||
        bit_accumulator.pending_capture()->processed_end != processed_end) {
      return false;
    }
    if (!hash_chain.release_capture(generation_id, processed_end)) return false;
    if (!bit_accumulator.release_capture(generation_id, processed_end)) {
      return false;
    }
    generation_.reset();
    return true;
  }

private:
  std::optional<CaptureGeneration> generation_{};
};

} // namespace fexma::snapshot_demo
