/**
 * @file test_bootstrap_restore.cpp
 * @brief All-or-nothing bootstrap and suffix-equivalence checks for Demo 006.
 */

#include <fexma/snapshot_demo/bootstrap.hpp>
#include <fexma/snapshot_demo/record.hpp>
#include <fexma/snapshot_demo/snapshot_sink.hpp>
#include <fexma/wal/format.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <span>
#include <vector>

using namespace fexma;

namespace {

constexpr wal::Position snapshot_position = 11;
constexpr wal::Position record_count = 40;
constexpr std::uint64_t first_sequence = 1000;
constexpr snapshot_demo::SnapshotIdentity identity{
    wal::StreamKind::Generic, 31, 7, 13, 6006};

using Records = std::vector<snapshot_demo::ApplicationPayload>;

[[nodiscard]] std::filesystem::path test_root() {
  return std::filesystem::temp_directory_path() /
         "fexma_snapshot_demo_bootstrap_restore";
}

[[nodiscard]] Records make_records() {
  Records records;
  records.reserve(record_count);
  for (wal::Position position = 0; position < record_count; ++position) {
    if (position == snapshot_position) {
      records.push_back(snapshot_demo::encode_save_snapshot(position));
      continue;
    }
    snapshot_demo::ApplicationData data{};
    for (std::size_t index = 0; index < data.size(); ++index) {
      data[index] = static_cast<std::byte>((position * 17u + index) & 0xffu);
    }
    records.push_back(snapshot_demo::encode_data(data));
  }
  return records;
}

class ReplaySource final {
public:
  explicit ReplaySource(const Records& records) noexcept : records_(&records) {}

  [[nodiscard]] wal::AccessResult
  try_view(wal::Position position) const noexcept {
    if (position >= records_->size()) {
      return {wal::ViewStatus::Unpublished, {}};
    }
    return {wal::ViewStatus::Ok,
            {position, first_sequence + position,
             std::span<const std::byte>{(*records_)[position]}}};
  }

private:
  const Records* records_{};
};

class PositionFaultSource final {
public:
  PositionFaultSource(const Records& records,
                      wal::Position supplied_position) noexcept
      : records_(&records), supplied_position_(supplied_position) {}

  [[nodiscard]] wal::AccessResult
  try_view(wal::Position requested_position) const noexcept {
    if (requested_position >= records_->size()) {
      return {wal::ViewStatus::Unpublished, {}};
    }
    return {wal::ViewStatus::Ok,
            {supplied_position_, first_sequence + requested_position,
             std::span<const std::byte>{(*records_)[requested_position]}}};
  }

private:
  const Records* records_{};
  wal::Position supplied_position_{};
};

template <class Module>
[[nodiscard]] bool process_range(Module& module, const Records& records,
                                 wal::Position begin,
                                 wal::Position end) noexcept {
  for (wal::Position position = begin; position < end; ++position) {
    if (!module.process({position, first_sequence + position,
                         std::span<const std::byte>{records[position]}})) {
      return false;
    }
  }
  return true;
}

template <std::size_t Size>
[[nodiscard]] bool read_bytes(const std::filesystem::path& path,
                              std::array<std::byte, Size>& bytes) {
  std::ifstream input(path, std::ios::binary);
  input.read(reinterpret_cast<char*>(bytes.data()),
             static_cast<std::streamsize>(bytes.size()));
  return input && input.gcount() == static_cast<std::streamsize>(bytes.size());
}

template <std::size_t Size>
[[nodiscard]] bool write_bytes(const std::filesystem::path& path,
                               const std::array<std::byte, Size>& bytes) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
  return output.good();
}

[[nodiscard]] bool create_snapshot(const Records& records,
                                   const std::filesystem::path& root,
                                   snapshot_demo::HashChainState& hash_state,
                                   snapshot_demo::BitAccumulatorState&
                                       bit_state) {
  snapshot_demo::HashChainModule hash;
  snapshot_demo::BitAccumulatorModule bits;
  if (!process_range(hash, records, 0, snapshot_position + 1u) ||
      !process_range(bits, records, 0, snapshot_position + 1u)) {
    return false;
  }

  snapshot_demo::CaptureGenerationCoordinator coordinator;
  if (coordinator.collect(hash, bits) !=
      snapshot_demo::CollectStatus::Complete) {
    return false;
  }
  const snapshot_demo::SnapshotSink sink(root, identity);
  if (!sink.save_and_release(coordinator, hash, bits).ok()) return false;
  hash_state = hash.state();
  bit_state = bits.state();
  return true;
}

[[nodiscard]] bool failed_restore_changes_nothing(
    const snapshot_demo::SnapshotLoader& loader,
    snapshot_demo::SnapshotLoadStatus expected_status,
    const Records& records) noexcept {
  ReplaySource source(records);
  wal::Progress upstream(record_count);
  wal::Progress hash_frontier(1);
  wal::Progress bit_frontier(1);
  snapshot_demo::HashChainModule hash;
  snapshot_demo::BitAccumulatorModule bits;
  if (!process_range(hash, records, 0, 1) ||
      !process_range(bits, records, 0, 1)) {
    return false;
  }
  wal::Slider hash_slider(source, upstream.reader(), hash_frontier.writer(),
                          hash, 1);
  wal::Slider bit_slider(source, hash_frontier.reader(), bit_frontier.writer(),
                         bits, 1);
  const auto original_hash = hash.state();
  const auto original_bits = bits.state();

  const snapshot_demo::SnapshotLoadStatus status =
      snapshot_demo::restore_snapshot_quiescent(
          loader, snapshot_position, hash, bits, hash_slider, hash_frontier,
          bit_slider, bit_frontier);
  return status == expected_status && hash.state() == original_hash &&
         bits.state() == original_bits && hash_slider.current() == 1 &&
         bit_slider.current() == 1 &&
         hash_frontier.reader().acquire() == 1 &&
         bit_frontier.reader().acquire() == 1;
}

[[nodiscard]] bool restore_matches_continuous_execution() {
  const Records records = make_records();
  const std::filesystem::path root = test_root();
  std::filesystem::remove_all(root);

  snapshot_demo::HashChainState captured_hash{};
  snapshot_demo::BitAccumulatorState captured_bits{};
  if (!create_snapshot(records, root, captured_hash, captured_bits)) {
    std::filesystem::remove_all(root);
    return false;
  }

  snapshot_demo::HashChainModule continuous_hash;
  snapshot_demo::BitAccumulatorModule continuous_bits;
  if (!process_range(continuous_hash, records, 0, record_count) ||
      !process_range(continuous_bits, records, 0, record_count)) {
    std::filesystem::remove_all(root);
    return false;
  }

  const snapshot_demo::SnapshotLoader loader(root, identity);
  const snapshot_demo::SnapshotLoadResult prepared =
      loader.load(snapshot_position);
  if (!prepared.ok() || prepared.prepared->hash_chain != captured_hash ||
      prepared.prepared->bit_accumulator != captured_bits ||
      prepared.prepared->description.processed_end != snapshot_position + 1u) {
    std::filesystem::remove_all(root);
    return false;
  }

  ReplaySource source(records);
  wal::Progress upstream(record_count);
  wal::Progress hash_frontier;
  wal::Progress bit_frontier;
  snapshot_demo::HashChainModule restored_hash;
  snapshot_demo::BitAccumulatorModule restored_bits;
  wal::Slider hash_slider(source, upstream.reader(), hash_frontier.writer(),
                          restored_hash);
  wal::Slider bit_slider(source, hash_frontier.reader(), bit_frontier.writer(),
                         restored_bits);

  if (snapshot_demo::restore_snapshot_quiescent(
          loader, snapshot_position, restored_hash, restored_bits, hash_slider,
          hash_frontier, bit_slider, bit_frontier) !=
          snapshot_demo::SnapshotLoadStatus::Ok ||
      hash_slider.current() != snapshot_position + 1u ||
      bit_slider.current() != snapshot_position + 1u ||
      hash_frontier.reader().acquire() != snapshot_position + 1u ||
      bit_frontier.reader().acquire() != snapshot_position + 1u ||
      restored_hash.state() != captured_hash ||
      restored_bits.state() != captured_bits ||
      restored_hash.pending_capture() != nullptr ||
      restored_bits.pending_capture() != nullptr) {
    std::filesystem::remove_all(root);
    return false;
  }

  const wal::SliderResult hash_result = hash_slider.process_available();
  const wal::SliderResult bit_result = bit_slider.process_available();
  const std::uint64_t suffix_size =
      record_count - snapshot_position - 1u;
  const bool valid = hash_result.status == wal::SliderStatus::Processed &&
                     bit_result.status == wal::SliderStatus::Processed &&
                     hash_result.processed_count == suffix_size &&
                     bit_result.processed_count == suffix_size &&
                     hash_frontier.reader().acquire() == record_count &&
                     bit_frontier.reader().acquire() == record_count &&
                     restored_hash.state() == continuous_hash.state() &&
                     restored_bits.state() == continuous_bits.state();
  std::filesystem::remove_all(root);
  return valid;
}

[[nodiscard]] bool position_fault_stops_at_resume(
    const snapshot_demo::SnapshotLoader& loader, const Records& records,
    wal::Position supplied_position) noexcept {
  const wal::Position resume_position = snapshot_position + 1u;
  PositionFaultSource source(records, supplied_position);
  wal::Progress upstream(resume_position + 1u);
  wal::Progress hash_frontier;
  wal::Progress bit_frontier;
  snapshot_demo::HashChainModule hash;
  snapshot_demo::BitAccumulatorModule bits;
  wal::Slider hash_slider(source, upstream.reader(), hash_frontier.writer(),
                          hash);
  wal::Slider bit_slider(source, hash_frontier.reader(), bit_frontier.writer(),
                         bits);

  if (snapshot_demo::restore_snapshot_quiescent(
          loader, snapshot_position, hash, bits, hash_slider, hash_frontier,
          bit_slider, bit_frontier) != snapshot_demo::SnapshotLoadStatus::Ok) {
    return false;
  }
  const snapshot_demo::BitAccumulatorState restored_bits = bits.state();
  const wal::SliderResult hash_result = hash_slider.process_available();
  const wal::SliderResult bit_result = bit_slider.process_available();
  return hash_result.status == wal::SliderStatus::ModuleFailed &&
         hash_result.processed_count == 0 && hash.state().failed &&
         hash.state().processed_end == resume_position &&
         hash_slider.current() == resume_position &&
         hash_frontier.reader().acquire() == resume_position &&
         bit_result.status == wal::SliderStatus::Empty &&
         bits.state() == restored_bits &&
         bit_slider.current() == resume_position &&
         bit_frontier.reader().acquire() == resume_position;
}

[[nodiscard]] bool invalid_snapshots_are_rejected_atomically() {
  const Records records = make_records();
  const std::filesystem::path root = test_root();
  std::filesystem::remove_all(root);
  snapshot_demo::HashChainState hash_state{};
  snapshot_demo::BitAccumulatorState bit_state{};
  if (!create_snapshot(records, root, hash_state, bit_state)) return false;

  const std::filesystem::path directory =
      root / ("snapshot-" + std::to_string(snapshot_position));
  const std::filesystem::path description_path =
      directory / "snapshot.description";
  const std::filesystem::path bit_path =
      directory / "bit_accumulator.snapshot";

  snapshot_demo::SnapshotIdentity wrong_identity = identity;
  ++wrong_identity.composition_id;
  const snapshot_demo::SnapshotLoader wrong_identity_loader(root,
                                                            wrong_identity);
  if (!failed_restore_changes_nothing(
          wrong_identity_loader,
          snapshot_demo::SnapshotLoadStatus::IdentityMismatch, records)) {
    std::filesystem::remove_all(root);
    return false;
  }

  const std::filesystem::path hidden_bit = directory / "bit.hidden";
  std::filesystem::rename(bit_path, hidden_bit);
  const snapshot_demo::SnapshotLoader loader(root, identity);
  const bool missing_is_atomic = failed_restore_changes_nothing(
      loader, snapshot_demo::SnapshotLoadStatus::ModuleMissing, records);
  std::filesystem::rename(hidden_bit, bit_path);
  if (!missing_is_atomic) {
    std::filesystem::remove_all(root);
    return false;
  }

  snapshot_demo::BitAccumulatorSnapshotBytes original_bits{};
  if (!read_bytes(bit_path, original_bits)) return false;
  auto corrupted_bits = original_bits;
  corrupted_bits[48] ^= std::byte{1};
  if (!write_bytes(bit_path, corrupted_bits) ||
      !failed_restore_changes_nothing(
          loader, snapshot_demo::SnapshotLoadStatus::ModuleChecksumMismatch,
          records) ||
      !write_bytes(bit_path, original_bits)) {
    std::filesystem::remove_all(root);
    return false;
  }

  snapshot_demo::SnapshotDescriptionBytes original_description{};
  if (!read_bytes(description_path, original_description)) return false;
  snapshot_demo::SnapshotDescription description{};
  if (!snapshot_demo::deserialize_snapshot_description(original_description,
                                                       description)) {
    return false;
  }

  snapshot_demo::BitAccumulatorCapture original_bit_capture{};
  if (!snapshot_demo::deserialize_bit_accumulator_snapshot(
          original_bits, original_bit_capture)) {
    std::filesystem::remove_all(root);
    return false;
  }
  snapshot_demo::BitAccumulatorCapture mismatched_capture =
      original_bit_capture;
  ++mismatched_capture.processed_end;
  ++mismatched_capture.state.processed_end;
  const auto mismatched_bits =
      snapshot_demo::serialize_bit_accumulator_snapshot(mismatched_capture);
  snapshot_demo::SnapshotDescription mismatched_description = description;
  mismatched_description.bit_accumulator.checksum =
      wal::crc32_bytes(mismatched_bits.data(), mismatched_bits.size());
  const auto mismatched_description_bytes =
      snapshot_demo::serialize_snapshot_description(mismatched_description);
  if (!write_bytes(bit_path, mismatched_bits) ||
      !write_bytes(description_path, mismatched_description_bytes) ||
      !failed_restore_changes_nothing(
          loader, snapshot_demo::SnapshotLoadStatus::CaptureBoundaryMismatch,
          records) ||
      !write_bytes(bit_path, original_bits) ||
      !write_bytes(description_path, original_description)) {
    std::filesystem::remove_all(root);
    return false;
  }

  auto invalid_encoding = original_bits;
  invalid_encoding.back() = std::byte{1};
  snapshot_demo::SnapshotDescription invalid_encoding_description =
      description;
  invalid_encoding_description.bit_accumulator.checksum =
      wal::crc32_bytes(invalid_encoding.data(), invalid_encoding.size());
  const auto invalid_encoding_description_bytes =
      snapshot_demo::serialize_snapshot_description(
          invalid_encoding_description);
  if (!write_bytes(bit_path, invalid_encoding) ||
      !write_bytes(description_path, invalid_encoding_description_bytes) ||
      !failed_restore_changes_nothing(
          loader, snapshot_demo::SnapshotLoadStatus::ModuleDecodeError,
          records) ||
      !write_bytes(bit_path, original_bits) ||
      !write_bytes(description_path, original_description)) {
    std::filesystem::remove_all(root);
    return false;
  }

  ++description.hash_chain.schema_version;
  const auto incompatible_description =
      snapshot_demo::serialize_snapshot_description(description);
  if (!write_bytes(description_path, incompatible_description) ||
      !failed_restore_changes_nothing(
          loader,
          snapshot_demo::SnapshotLoadStatus::ModuleDescriptionInvalid,
          records) ||
      !write_bytes(description_path, original_description)) {
    std::filesystem::remove_all(root);
    return false;
  }

  const std::filesystem::path hidden_description =
      directory / "description.hidden";
  std::filesystem::rename(description_path, hidden_description);
  const bool missing_description = failed_restore_changes_nothing(
      loader, snapshot_demo::SnapshotLoadStatus::DescriptionMissing, records);
  std::filesystem::rename(hidden_description, description_path);
  const bool repeat_rejected =
      position_fault_stops_at_resume(loader, records, snapshot_position);
  const bool omission_rejected = position_fault_stops_at_resume(
      loader, records, snapshot_position + 2u);
  std::filesystem::remove_all(root);
  return missing_description && repeat_rejected && omission_rejected;
}

} // namespace

int main() {
  if (!restore_matches_continuous_execution()) return 1;
  if (!invalid_snapshots_are_rejected_atomically()) return 2;
  return 0;
}
