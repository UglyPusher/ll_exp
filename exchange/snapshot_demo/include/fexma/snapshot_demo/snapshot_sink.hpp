#pragma once

/**
 * @file snapshot_sink.hpp
 * @brief Composition-level durable snapshot publication for Demo 006.
 */

#include <fexma/snapshot_demo/snapshot_format.hpp>

#include <cstdint>
#include <filesystem>

namespace fexma::snapshot_demo {

struct SnapshotSaveTimings {
  std::uint64_t hash_capture_ns{};
  std::uint64_t bit_accumulator_capture_ns{};
  std::uint64_t generation_completion_ns{};
  std::uint64_t serialization_ns{};
  std::uint64_t write_ns{};
  std::uint64_t flush_ns{};
  std::uint64_t fsync_ns{};
  std::uint64_t publication_ns{};
  std::uint64_t total_ns{};
};

enum class SnapshotSaveStatus {
  Ok,
  InvalidConfig,
  InvalidGeneration,
  GenerationUnavailable,
  AlreadyPublished,
  DirectoryError,
  OpenError,
  WriteError,
  FlushError,
  SyncError,
  CloseError,
  PublicationError,
  ReleaseError,
};

struct SnapshotSaveResult {
  SnapshotSaveStatus status{SnapshotSaveStatus::InvalidConfig};
  SnapshotSaveTimings timings{};

  [[nodiscard]] bool ok() const noexcept {
    return status == SnapshotSaveStatus::Ok;
  }
};

class SnapshotSink final {
public:
  SnapshotSink(std::filesystem::path root,
               SnapshotIdentity identity) noexcept;

  [[nodiscard]] SnapshotSaveResult
  save(const CaptureGeneration& generation) const noexcept;

  [[nodiscard]] SnapshotSaveResult save_and_release(
      CaptureGenerationCoordinator& coordinator, HashChainModule& hash_chain,
      BitAccumulatorModule& bit_accumulator) const noexcept;

  [[nodiscard]] const std::filesystem::path& root() const noexcept {
    return root_;
  }

  [[nodiscard]] const SnapshotIdentity& identity() const noexcept {
    return identity_;
  }

private:
  std::filesystem::path root_{};
  SnapshotIdentity identity_{};
};

} // namespace fexma::snapshot_demo
