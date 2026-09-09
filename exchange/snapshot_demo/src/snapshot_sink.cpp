#include <fexma/snapshot_demo/snapshot_sink.hpp>

#include "snapshot_sink_test_access.hpp"

#include <fexma/wal/format.hpp>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <span>
#include <string>
#include <system_error>
#include <utility>

#if defined(_WIN32)
#include <io.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace fexma::snapshot_demo {
namespace {

using Clock = std::chrono::steady_clock;

detail::SnapshotSinkTestControl* test_control = nullptr;

[[nodiscard]] std::uint64_t elapsed_ns(Clock::time_point started) noexcept {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() -
                                                           started)
          .count());
}

[[nodiscard]] bool valid_identity(const SnapshotIdentity& identity) noexcept {
  const bool valid_stream_kind =
      identity.stream_kind == wal::StreamKind::Generic ||
      identity.stream_kind == wal::StreamKind::Command ||
      identity.stream_kind == wal::StreamKind::Event;
  const bool valid_required_ids =
      identity.stream_kind == wal::StreamKind::Generic ||
      (identity.stream_id != 0 && identity.epoch_id != 0 &&
       identity.manifest_id != 0);
  return valid_stream_kind && valid_required_ids &&
         identity.composition_id != 0;
}

[[nodiscard]] bool
valid_generation(const CaptureGeneration& generation) noexcept {
  if (generation.record_position ==
      std::numeric_limits<wal::Position>::max()) {
    return false;
  }
  const wal::Position expected_end = generation.record_position + 1u;
  return generation.generation_id == generation.record_position &&
         generation.processed_end == expected_end &&
         generation.hash_chain.generation_id == generation.generation_id &&
         generation.bit_accumulator.generation_id ==
             generation.generation_id &&
         generation.hash_chain.record_position ==
             generation.record_position &&
         generation.bit_accumulator.record_position ==
             generation.record_position &&
         generation.hash_chain.processed_end == generation.processed_end &&
         generation.bit_accumulator.processed_end ==
             generation.processed_end &&
         generation.hash_chain.sequence == generation.sequence &&
         generation.bit_accumulator.sequence == generation.sequence &&
         generation.hash_chain.state.processed_end ==
             generation.processed_end &&
         generation.bit_accumulator.state.processed_end ==
             generation.processed_end &&
         !generation.hash_chain.state.failed &&
         !generation.bit_accumulator.state.failed;
}

[[nodiscard]] std::FILE*
open_binary_file(const std::filesystem::path& path) noexcept {
  if (test_control != nullptr && test_control->fail_open) return nullptr;
#if defined(_WIN32)
  std::FILE* file = nullptr;
  return ::_wfopen_s(&file, path.c_str(), L"wb") == 0 ? file : nullptr;
#else
  return std::fopen(path.c_str(), "wb");
#endif
}

[[nodiscard]] SnapshotSaveStatus
write_and_sync_file(const std::filesystem::path& path,
                    std::span<const std::byte> bytes,
                    SnapshotSaveTimings& timings) noexcept {
  std::FILE* file = open_binary_file(path);
  if (file == nullptr) return SnapshotSaveStatus::OpenError;

  const auto write_started = Clock::now();
  const std::size_t written =
      test_control != nullptr && test_control->fail_write
          ? 0u
          : std::fwrite(bytes.data(), 1u, bytes.size(), file);
  timings.write_ns += elapsed_ns(write_started);
  if (written != bytes.size()) {
    std::fclose(file);
    return SnapshotSaveStatus::WriteError;
  }

  const auto flush_started = Clock::now();
  const bool flushed =
      !(test_control != nullptr && test_control->fail_flush) &&
      std::fflush(file) == 0;
  timings.flush_ns += elapsed_ns(flush_started);
  if (!flushed) {
    std::fclose(file);
    return SnapshotSaveStatus::FlushError;
  }

  const auto sync_started = Clock::now();
#if defined(_WIN32)
  const bool synced =
      !(test_control != nullptr && test_control->fail_sync) &&
      ::_commit(::_fileno(file)) == 0;
#else
  const bool synced =
      !(test_control != nullptr && test_control->fail_sync) &&
      ::fsync(::fileno(file)) == 0;
#endif
  timings.fsync_ns += elapsed_ns(sync_started);
  if (!synced) {
    std::fclose(file);
    return SnapshotSaveStatus::SyncError;
  }

  return std::fclose(file) == 0 ? SnapshotSaveStatus::Ok
                                : SnapshotSaveStatus::CloseError;
}

[[nodiscard]] bool sync_directory(const std::filesystem::path& path,
                                  SnapshotSaveTimings& timings) noexcept {
#if defined(_WIN32)
  static_cast<void>(path);
  static_cast<void>(timings);
  return true;
#else
  const auto sync_started = Clock::now();
  const int descriptor = ::open(path.c_str(), O_RDONLY | O_DIRECTORY);
  const bool synced = descriptor != -1 && ::fsync(descriptor) == 0;
  const bool closed = descriptor == -1 || ::close(descriptor) == 0;
  timings.fsync_ns += elapsed_ns(sync_started);
  return synced && closed;
#endif
}

[[nodiscard]] bool rename_path(const std::filesystem::path& from,
                               const std::filesystem::path& to,
                               SnapshotSaveTimings& timings) noexcept {
  const auto publication_started = Clock::now();
  std::error_code error;
  std::filesystem::rename(from, to, error);
  timings.publication_ns += elapsed_ns(publication_started);
  return !error;
}

[[nodiscard]] SnapshotSaveResult finish(SnapshotSaveStatus status,
                                        SnapshotSaveTimings timings,
                                        Clock::time_point started) noexcept {
  timings.total_ns = elapsed_ns(started);
  return {status, timings};
}

} // namespace

namespace detail {

void set_snapshot_sink_test_control(SnapshotSinkTestControl* control) noexcept {
  test_control = control;
}

} // namespace detail

SnapshotSink::SnapshotSink(std::filesystem::path root,
                           SnapshotIdentity identity) noexcept
    : root_(std::move(root)), identity_(identity) {}

SnapshotSaveResult
SnapshotSink::save(const CaptureGeneration& generation) const noexcept {
  const auto total_started = Clock::now();
  SnapshotSaveTimings timings{
      generation.hash_capture_duration_ns,
      generation.bit_accumulator_capture_duration_ns,
      generation.generation_completion_duration_ns};

  if (root_.empty() || !valid_identity(identity_)) {
    return finish(SnapshotSaveStatus::InvalidConfig, timings, total_started);
  }
  if (!valid_generation(generation)) {
    return finish(SnapshotSaveStatus::InvalidGeneration, timings,
                  total_started);
  }

  try {
    const std::string generation_name =
        "snapshot-" + std::to_string(generation.generation_id);
    const std::filesystem::path published = root_ / generation_name;
    const std::filesystem::path staging =
        root_ / (generation_name + ".pending");
    const std::filesystem::path hash_path = staging / "hash_chain.snapshot";
    const std::filesystem::path bit_path =
        staging / "bit_accumulator.snapshot";
    const std::filesystem::path pending_description =
        staging / "snapshot.description.pending";
    const std::filesystem::path final_description =
        staging / "snapshot.description";

    std::error_code error;
    std::filesystem::create_directories(root_, error);
    if (error || !std::filesystem::is_directory(root_, error) || error) {
      return finish(SnapshotSaveStatus::DirectoryError, timings,
                    total_started);
    }
    if (std::filesystem::exists(published, error)) {
      return finish(error ? SnapshotSaveStatus::DirectoryError
                          : SnapshotSaveStatus::AlreadyPublished,
                    timings, total_started);
    }
    if (error) {
      return finish(SnapshotSaveStatus::DirectoryError, timings,
                    total_started);
    }
    std::filesystem::remove_all(staging, error);
    if (error || !std::filesystem::create_directory(staging, error) || error) {
      return finish(SnapshotSaveStatus::DirectoryError, timings,
                    total_started);
    }

    const auto serialization_started = Clock::now();
    const HashChainSnapshotBytes hash_bytes =
        serialize_hash_chain_snapshot(generation.hash_chain);
    const BitAccumulatorSnapshotBytes bit_bytes =
        serialize_bit_accumulator_snapshot(generation.bit_accumulator);
    const SnapshotDescription description{
        identity_,
        generation.generation_id,
        generation.record_position,
        generation.processed_end,
        generation.sequence,
        {kHashChainModuleId, kHashChainSchemaVersion,
         SnapshotFileKind::HashChain, hash_bytes.size(),
         wal::crc32_bytes(hash_bytes.data(), hash_bytes.size())},
        {kBitAccumulatorModuleId, kBitAccumulatorSchemaVersion,
         SnapshotFileKind::BitAccumulator, bit_bytes.size(),
         wal::crc32_bytes(bit_bytes.data(), bit_bytes.size())}};
    const SnapshotDescriptionBytes description_bytes =
        serialize_snapshot_description(description);
    timings.serialization_ns += elapsed_ns(serialization_started);

    SnapshotSaveStatus status =
        write_and_sync_file(hash_path, hash_bytes, timings);
    if (status != SnapshotSaveStatus::Ok) {
      return finish(status, timings, total_started);
    }
    status = write_and_sync_file(bit_path, bit_bytes, timings);
    if (status != SnapshotSaveStatus::Ok) {
      return finish(status, timings, total_started);
    }
    status =
        write_and_sync_file(pending_description, description_bytes, timings);
    if (status != SnapshotSaveStatus::Ok) {
      return finish(status, timings, total_started);
    }

    if (test_control != nullptr && test_control->fail_publication) {
      return finish(SnapshotSaveStatus::PublicationError, timings,
                    total_started);
    }
    if (!rename_path(pending_description, final_description, timings) ||
        !sync_directory(staging, timings) ||
        !rename_path(staging, published, timings) ||
        !sync_directory(root_, timings)) {
      return finish(SnapshotSaveStatus::PublicationError, timings,
                    total_started);
    }
    return finish(SnapshotSaveStatus::Ok, timings, total_started);
  } catch (...) {
    return finish(SnapshotSaveStatus::DirectoryError, timings, total_started);
  }
}

SnapshotSaveResult SnapshotSink::save_and_release(
    CaptureGenerationCoordinator& coordinator, HashChainModule& hash_chain,
    BitAccumulatorModule& bit_accumulator) const noexcept {
  const CaptureGeneration* generation = coordinator.pending_generation();
  if (generation == nullptr) {
    return {SnapshotSaveStatus::GenerationUnavailable, {}};
  }
  const std::uint64_t generation_id = generation->generation_id;
  SnapshotSaveResult result = save(*generation);
  if (!result.ok()) return result;
  if (!coordinator.release_generation(hash_chain, bit_accumulator,
                                      generation_id)) {
    result.status = SnapshotSaveStatus::ReleaseError;
  }
  return result;
}

} // namespace fexma::snapshot_demo
