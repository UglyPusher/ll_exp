#pragma once

/**
 * @file bootstrap.hpp
 * @brief All-or-nothing quiescent publication of a prepared Demo 006 state.
 */

#include <fexma/snapshot_demo/snapshot_loader.hpp>
#include <fexma/wal/record_tape_types.hpp>
#include <fexma/wal/slider.hpp>

#include <cstdint>

namespace fexma::snapshot_demo {

template <class HashSlider, class BitAccumulatorSlider>
[[nodiscard]] SnapshotLoadStatus restore_snapshot_quiescent(
    const SnapshotLoader& loader, std::uint64_t generation_id,
    HashChainModule& hash_chain, BitAccumulatorModule& bit_accumulator,
    HashSlider& hash_slider,
    BitAccumulatorSlider& bit_accumulator_slider) noexcept {
  SnapshotLoadResult loaded = loader.load(generation_id);
  if (!loaded.ok()) return loaded.status;

  const PreparedSnapshot& prepared = *loaded.prepared;
  const wal::Position resume_position = prepared.description.processed_end;

  // Bootstrap is quiescent. Validation has completed for every participant,
  // so these non-failing assignments publish one prepared composition.
  hash_chain.restore_quiescent(prepared.hash_chain);
  bit_accumulator.restore_quiescent(prepared.bit_accumulator);
  hash_slider.reset_quiescent(resume_position);
  bit_accumulator_slider.reset_quiescent(resume_position);
  return SnapshotLoadStatus::Ok;
}

} // namespace fexma::snapshot_demo
