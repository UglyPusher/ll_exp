/**
 * @file test_snapshot_sink.cpp
 * @brief Durable full-generation publication checks for Demo 006.
 */

#include <fexma/snapshot_demo/snapshot_sink.hpp>

#include "snapshot_sink_test_access.hpp"

#include <fexma/snapshot_demo/record.hpp>
#include <fexma/wal/format.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>
#include <string>
#include <vector>

using namespace fexma;

namespace {

[[nodiscard]] std::filesystem::path test_root() {
  return std::filesystem::temp_directory_path() /
         "fexma_snapshot_demo_snapshot_sink";
}

[[nodiscard]] std::vector<std::byte>
read_file(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  const std::vector<char> chars{std::istreambuf_iterator<char>(input),
                                std::istreambuf_iterator<char>()};
  std::vector<std::byte> bytes(chars.size());
  for (std::size_t index = 0; index < chars.size(); ++index) {
    bytes[index] = static_cast<std::byte>(
        static_cast<unsigned char>(chars[index]));
  }
  return bytes;
}

[[nodiscard]] bool make_generation(
    snapshot_demo::HashChainModule& hash,
    snapshot_demo::BitAccumulatorModule& bits,
    snapshot_demo::CaptureGenerationCoordinator& coordinator) noexcept {
  snapshot_demo::ApplicationData data{};
  for (std::size_t index = 0; index < data.size(); ++index) {
    data[index] = static_cast<std::byte>(index + 3u);
  }
  const auto ordinary = snapshot_demo::encode_data(data);
  const auto snapshot = snapshot_demo::encode_save_snapshot(1);

  if (!hash.process({0, 100, ordinary}) ||
      !bits.process({0, 100, ordinary}) ||
      !hash.process({1, 101, snapshot}) ||
      !bits.process({1, 101, snapshot}) ||
      coordinator.collect(hash, bits) !=
          snapshot_demo::CollectStatus::Complete) {
    return false;
  }
  return true;
}

[[nodiscard]] bool same_persisted_capture(
    const snapshot_demo::HashChainCapture& expected,
    const snapshot_demo::HashChainCapture& actual) noexcept {
  return expected == actual;
}

[[nodiscard]] bool same_persisted_capture(
    const snapshot_demo::BitAccumulatorCapture& expected,
    const snapshot_demo::BitAccumulatorCapture& actual) noexcept {
  return expected == actual;
}

[[nodiscard]] bool sink_publishes_complete_generation_last() {
  const std::filesystem::path root = test_root();
  std::filesystem::remove_all(root);

  snapshot_demo::HashChainModule hash;
  snapshot_demo::BitAccumulatorModule bits;
  snapshot_demo::CaptureGenerationCoordinator coordinator;
  if (!make_generation(hash, bits, coordinator)) return false;
  const snapshot_demo::CaptureGeneration generation =
      *coordinator.pending_generation();
  if (generation.processed_end != 2) return false;

  constexpr snapshot_demo::SnapshotIdentity identity{
      wal::StreamKind::Generic, 31, 7, 13, 6006};
  const snapshot_demo::SnapshotSink sink(root, identity);
  const std::filesystem::path staging = root / "snapshot-1.pending";
  const std::filesystem::path published = root / "snapshot-1";

  snapshot_demo::detail::SnapshotSinkTestControl control{};
  control.fail_publication = true;
  snapshot_demo::detail::set_snapshot_sink_test_control(&control);
  const snapshot_demo::SnapshotSaveResult interrupted =
      sink.save_and_release(coordinator, hash, bits);
  snapshot_demo::detail::set_snapshot_sink_test_control(nullptr);
  if (interrupted.status != snapshot_demo::SnapshotSaveStatus::PublicationError ||
      std::filesystem::exists(published) ||
      std::filesystem::exists(staging / "snapshot.description") ||
      coordinator.pending_generation() == nullptr ||
      hash.pending_capture() == nullptr || bits.pending_capture() == nullptr) {
    std::filesystem::remove_all(root);
    return false;
  }

  const snapshot_demo::SnapshotSaveResult saved =
      sink.save_and_release(coordinator, hash, bits);
  if (!saved.ok() || std::filesystem::exists(staging) ||
      !std::filesystem::is_regular_file(published / "hash_chain.snapshot") ||
      !std::filesystem::is_regular_file(
          published / "bit_accumulator.snapshot") ||
      !std::filesystem::is_regular_file(published / "snapshot.description") ||
      saved.timings.hash_capture_ns !=
          generation.hash_capture_duration_ns ||
      saved.timings.bit_accumulator_capture_ns !=
          generation.bit_accumulator_capture_duration_ns ||
      saved.timings.generation_completion_ns !=
          generation.generation_completion_duration_ns ||
      saved.timings.total_ns == 0 ||
      coordinator.pending_generation() != nullptr ||
      hash.pending_capture() != nullptr || bits.pending_capture() != nullptr) {
    std::filesystem::remove_all(root);
    return false;
  }

  std::size_t entry_count = 0;
  for (const auto& entry : std::filesystem::directory_iterator(published)) {
    static_cast<void>(entry);
    ++entry_count;
  }
  if (entry_count != 3) {
    std::filesystem::remove_all(root);
    return false;
  }

  const std::vector<std::byte> hash_bytes =
      read_file(published / "hash_chain.snapshot");
  const std::vector<std::byte> bit_bytes =
      read_file(published / "bit_accumulator.snapshot");
  const std::vector<std::byte> description_bytes =
      read_file(published / "snapshot.description");

  snapshot_demo::SnapshotDescription description{};
  snapshot_demo::HashChainCapture restored_hash{};
  snapshot_demo::BitAccumulatorCapture restored_bits{};
  if (!snapshot_demo::deserialize_snapshot_description(description_bytes,
                                                       description) ||
      !snapshot_demo::deserialize_hash_chain_snapshot(hash_bytes,
                                                      restored_hash) ||
      !snapshot_demo::deserialize_bit_accumulator_snapshot(bit_bytes,
                                                           restored_bits)) {
    std::filesystem::remove_all(root);
    return false;
  }

  const bool valid =
      description.identity == identity && description.generation_id == 1 &&
      description.record_position == 1 && description.processed_end == 2 &&
      description.snapshot_sequence == 101 &&
      description.hash_chain.module_id == snapshot_demo::kHashChainModuleId &&
      description.hash_chain.schema_version ==
          snapshot_demo::kHashChainSchemaVersion &&
      description.hash_chain.file_size == hash_bytes.size() &&
      description.hash_chain.checksum ==
          wal::crc32_bytes(hash_bytes.data(), hash_bytes.size()) &&
      description.bit_accumulator.module_id ==
          snapshot_demo::kBitAccumulatorModuleId &&
      description.bit_accumulator.schema_version ==
          snapshot_demo::kBitAccumulatorSchemaVersion &&
      description.bit_accumulator.file_size == bit_bytes.size() &&
      description.bit_accumulator.checksum ==
          wal::crc32_bytes(bit_bytes.data(), bit_bytes.size()) &&
      same_persisted_capture(generation.hash_chain, restored_hash) &&
      same_persisted_capture(generation.bit_accumulator, restored_bits) &&
      sink.save(generation).status ==
          snapshot_demo::SnapshotSaveStatus::AlreadyPublished;

  auto corrupted_description = description_bytes;
  corrupted_description[40] ^= std::byte{1};
  snapshot_demo::SnapshotDescription rejected{};
  const bool corruption_rejected =
      !snapshot_demo::deserialize_snapshot_description(corrupted_description,
                                                       rejected);
  std::filesystem::remove_all(root);
  return valid && corruption_rejected;
}

[[nodiscard]] bool invalid_input_is_rejected() {
  const std::filesystem::path root = test_root();
  std::filesystem::remove_all(root);
  snapshot_demo::HashChainModule hash;
  snapshot_demo::BitAccumulatorModule bits;
  snapshot_demo::CaptureGenerationCoordinator coordinator;
  if (!make_generation(hash, bits, coordinator)) return false;
  const snapshot_demo::CaptureGeneration generation =
      *coordinator.pending_generation();

  const snapshot_demo::SnapshotSink invalid_sink(root, {});
  if (invalid_sink.save(generation).status !=
      snapshot_demo::SnapshotSaveStatus::InvalidConfig) {
    return false;
  }

  snapshot_demo::CaptureGeneration invalid_generation = generation;
  invalid_generation.bit_accumulator.processed_end = 3;
  constexpr snapshot_demo::SnapshotIdentity identity{
      wal::StreamKind::Generic, 31, 7, 13, 6006};
  const snapshot_demo::SnapshotSink sink(root, identity);
  const bool rejected =
      sink.save(invalid_generation).status ==
          snapshot_demo::SnapshotSaveStatus::InvalidGeneration &&
      !std::filesystem::exists(root) &&
      snapshot_demo::SnapshotSink(root, identity)
              .save_and_release(coordinator, hash, bits)
              .ok() &&
      sink.save_and_release(coordinator, hash, bits).status ==
          snapshot_demo::SnapshotSaveStatus::GenerationUnavailable;
  std::filesystem::remove_all(root);
  return rejected;
}

[[nodiscard]] bool io_failure_preserves_generation(
    int failure_index,
    snapshot_demo::SnapshotSaveStatus expected_status) {
  const std::filesystem::path root =
      test_root().string() + "_failure_" + std::to_string(failure_index);
  std::filesystem::remove_all(root);

  snapshot_demo::HashChainModule hash;
  snapshot_demo::BitAccumulatorModule bits;
  snapshot_demo::CaptureGenerationCoordinator coordinator;
  if (!make_generation(hash, bits, coordinator)) return false;

  constexpr snapshot_demo::SnapshotIdentity identity{
      wal::StreamKind::Generic, 31, 7, 13, 6006};
  const snapshot_demo::SnapshotSink sink(root, identity);
  snapshot_demo::detail::SnapshotSinkTestControl control{};
  if (failure_index == 0) control.fail_open = true;
  if (failure_index == 1) control.fail_write = true;
  if (failure_index == 2) control.fail_flush = true;
  if (failure_index == 3) control.fail_sync = true;
  snapshot_demo::detail::set_snapshot_sink_test_control(&control);
  const snapshot_demo::SnapshotSaveResult failed =
      sink.save_and_release(coordinator, hash, bits);
  snapshot_demo::detail::set_snapshot_sink_test_control(nullptr);

  const bool retained = failed.status == expected_status &&
                        coordinator.pending_generation() != nullptr &&
                        hash.pending_capture() != nullptr &&
                        bits.pending_capture() != nullptr &&
                        !std::filesystem::exists(root / "snapshot-1");
  const bool retried = retained &&
                       sink.save_and_release(coordinator, hash, bits).ok() &&
                       coordinator.pending_generation() == nullptr &&
                       hash.pending_capture() == nullptr &&
                       bits.pending_capture() == nullptr;
  std::filesystem::remove_all(root);
  return retried;
}

} // namespace

int main() {
  if (!invalid_input_is_rejected()) return 1;
  if (!sink_publishes_complete_generation_last()) return 2;
  if (!io_failure_preserves_generation(
          0, snapshot_demo::SnapshotSaveStatus::OpenError)) {
    return 3;
  }
  if (!io_failure_preserves_generation(
          1, snapshot_demo::SnapshotSaveStatus::WriteError)) {
    return 4;
  }
  if (!io_failure_preserves_generation(
          2, snapshot_demo::SnapshotSaveStatus::FlushError)) {
    return 5;
  }
  if (!io_failure_preserves_generation(
          3, snapshot_demo::SnapshotSaveStatus::SyncError)) {
    return 6;
  }
  return 0;
}
