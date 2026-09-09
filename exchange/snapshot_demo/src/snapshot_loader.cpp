#include <fexma/snapshot_demo/snapshot_loader.hpp>

#include <fexma/wal/format.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <system_error>
#include <utility>

namespace fexma::snapshot_demo {
namespace {

enum class FileReadStatus {
  Ok,
  Missing,
  SizeMismatch,
  ReadError,
};

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

template <std::size_t Size>
[[nodiscard]] FileReadStatus
read_fixed_file(const std::filesystem::path& path,
                std::array<std::byte, Size>& bytes) noexcept {
  std::error_code error;
  const bool exists = std::filesystem::exists(path, error);
  if (error) return FileReadStatus::ReadError;
  if (!exists) return FileReadStatus::Missing;
  if (!std::filesystem::is_regular_file(path, error) || error) {
    return FileReadStatus::ReadError;
  }

  const std::uintmax_t size = std::filesystem::file_size(path, error);
  if (error) return FileReadStatus::ReadError;
  if (size != Size) return FileReadStatus::SizeMismatch;

  std::ifstream input(path, std::ios::binary);
  if (!input) return FileReadStatus::ReadError;
  input.read(reinterpret_cast<char*>(bytes.data()),
             static_cast<std::streamsize>(bytes.size()));
  return input && input.gcount() == static_cast<std::streamsize>(bytes.size())
             ? FileReadStatus::Ok
             : FileReadStatus::ReadError;
}

[[nodiscard]] bool valid_module_descriptions(
    const SnapshotDescription& description) noexcept {
  return description.hash_chain.module_id == kHashChainModuleId &&
         description.hash_chain.schema_version ==
             kHashChainSchemaVersion &&
         description.hash_chain.file_kind == SnapshotFileKind::HashChain &&
         description.bit_accumulator.module_id ==
             kBitAccumulatorModuleId &&
         description.bit_accumulator.schema_version ==
             kBitAccumulatorSchemaVersion &&
         description.bit_accumulator.file_kind ==
             SnapshotFileKind::BitAccumulator;
}

[[nodiscard]] bool valid_description_boundary(
    const SnapshotDescription& description,
    std::uint64_t requested_generation) noexcept {
  return description.record_position !=
             std::numeric_limits<wal::Position>::max() &&
         description.generation_id == requested_generation &&
         description.record_position == requested_generation &&
         description.processed_end == description.record_position + 1u;
}

[[nodiscard]] bool captures_match_description(
    const SnapshotDescription& description, const HashChainCapture& hash,
    const BitAccumulatorCapture& bits) noexcept {
  return hash.generation_id == description.generation_id &&
         bits.generation_id == description.generation_id &&
         hash.record_position == description.record_position &&
         bits.record_position == description.record_position &&
         hash.processed_end == description.processed_end &&
         bits.processed_end == description.processed_end &&
         hash.sequence == description.snapshot_sequence &&
         bits.sequence == description.snapshot_sequence &&
         hash.state.processed_end == description.processed_end &&
         bits.state.processed_end == description.processed_end;
}

} // namespace

SnapshotLoader::SnapshotLoader(std::filesystem::path root,
                               SnapshotIdentity expected_identity) noexcept
    : root_(std::move(root)), expected_identity_(expected_identity) {}

SnapshotLoadResult SnapshotLoader::load(std::uint64_t generation_id) const
    noexcept {
  if (root_.empty() || !valid_identity(expected_identity_)) {
    return {SnapshotLoadStatus::InvalidConfig, {}};
  }

  try {
    const std::filesystem::path directory =
        root_ / ("snapshot-" + std::to_string(generation_id));
    SnapshotDescriptionBytes description_bytes{};
    const FileReadStatus description_read = read_fixed_file(
        directory / "snapshot.description", description_bytes);
    if (description_read == FileReadStatus::Missing) {
      return {SnapshotLoadStatus::DescriptionMissing, {}};
    }
    if (description_read == FileReadStatus::SizeMismatch) {
      return {SnapshotLoadStatus::DescriptionInvalid, {}};
    }
    if (description_read != FileReadStatus::Ok) {
      return {SnapshotLoadStatus::DescriptionReadError, {}};
    }

    SnapshotDescription description{};
    if (!deserialize_snapshot_description(description_bytes, description)) {
      return {SnapshotLoadStatus::DescriptionInvalid, {}};
    }
    if (description.identity != expected_identity_) {
      return {SnapshotLoadStatus::IdentityMismatch, {}};
    }
    if (!valid_description_boundary(description, generation_id)) {
      return {SnapshotLoadStatus::GenerationMismatch, {}};
    }
    if (!valid_module_descriptions(description)) {
      return {SnapshotLoadStatus::ModuleDescriptionInvalid, {}};
    }
    if (description.hash_chain.file_size != kHashChainSnapshotSize ||
        description.bit_accumulator.file_size !=
            kBitAccumulatorSnapshotSize) {
      return {SnapshotLoadStatus::ModuleSizeMismatch, {}};
    }

    HashChainSnapshotBytes hash_bytes{};
    BitAccumulatorSnapshotBytes bit_bytes{};
    const FileReadStatus hash_read = read_fixed_file(
        directory / "hash_chain.snapshot", hash_bytes);
    const FileReadStatus bit_read = read_fixed_file(
        directory / "bit_accumulator.snapshot", bit_bytes);
    if (hash_read == FileReadStatus::Missing ||
        bit_read == FileReadStatus::Missing) {
      return {SnapshotLoadStatus::ModuleMissing, {}};
    }
    if (hash_read == FileReadStatus::SizeMismatch ||
        bit_read == FileReadStatus::SizeMismatch) {
      return {SnapshotLoadStatus::ModuleSizeMismatch, {}};
    }
    if (hash_read != FileReadStatus::Ok || bit_read != FileReadStatus::Ok) {
      return {SnapshotLoadStatus::ModuleReadError, {}};
    }

    if (wal::crc32_bytes(hash_bytes.data(), hash_bytes.size()) !=
            description.hash_chain.checksum ||
        wal::crc32_bytes(bit_bytes.data(), bit_bytes.size()) !=
            description.bit_accumulator.checksum) {
      return {SnapshotLoadStatus::ModuleChecksumMismatch, {}};
    }

    HashChainCapture hash{};
    BitAccumulatorCapture bits{};
    if (!deserialize_hash_chain_snapshot(hash_bytes, hash) ||
        !deserialize_bit_accumulator_snapshot(bit_bytes, bits)) {
      return {SnapshotLoadStatus::ModuleDecodeError, {}};
    }
    if (!captures_match_description(description, hash, bits)) {
      return {SnapshotLoadStatus::CaptureBoundaryMismatch, {}};
    }
    if (hash.state.failed || bits.state.failed) {
      return {SnapshotLoadStatus::CapturedStateInvalid, {}};
    }

    return {SnapshotLoadStatus::Ok,
            PreparedSnapshot{description, hash.state, bits.state}};
  } catch (...) {
    return {SnapshotLoadStatus::DescriptionReadError, {}};
  }
}

} // namespace fexma::snapshot_demo
