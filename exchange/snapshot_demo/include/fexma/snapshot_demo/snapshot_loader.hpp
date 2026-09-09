#pragma once

/**
 * @file snapshot_loader.hpp
 * @brief Isolated bootstrap preparation for Demo 006 snapshots.
 */

#include <fexma/snapshot_demo/snapshot_format.hpp>

#include <cstdint>
#include <filesystem>
#include <optional>

namespace fexma::snapshot_demo {

struct PreparedSnapshot {
  SnapshotDescription description{};
  HashChainState hash_chain{};
  BitAccumulatorState bit_accumulator{};
};

enum class SnapshotLoadStatus {
  Ok,
  InvalidConfig,
  DescriptionMissing,
  DescriptionReadError,
  DescriptionInvalid,
  IdentityMismatch,
  GenerationMismatch,
  ModuleDescriptionInvalid,
  ModuleMissing,
  ModuleReadError,
  ModuleSizeMismatch,
  ModuleChecksumMismatch,
  ModuleDecodeError,
  CaptureBoundaryMismatch,
  CapturedStateInvalid,
};

struct SnapshotLoadResult {
  SnapshotLoadStatus status{SnapshotLoadStatus::InvalidConfig};
  std::optional<PreparedSnapshot> prepared{};

  [[nodiscard]] bool ok() const noexcept {
    return status == SnapshotLoadStatus::Ok && prepared.has_value();
  }
};

class SnapshotLoader final {
public:
  SnapshotLoader(std::filesystem::path root,
                 SnapshotIdentity expected_identity) noexcept;

  [[nodiscard]] SnapshotLoadResult load(std::uint64_t generation_id) const
      noexcept;

private:
  std::filesystem::path root_{};
  SnapshotIdentity expected_identity_{};
};

} // namespace fexma::snapshot_demo
