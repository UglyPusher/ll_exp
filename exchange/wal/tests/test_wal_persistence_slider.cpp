/**
 * @file test_wal_persistence_slider.cpp
 * @brief Contract tests for persistence attached through the generic slider.
 */

#include <fexma/wal/noop_module.hpp>
#include <fexma/wal/persistence_slider.hpp>
#include <fexma/wal/reader.hpp>

#include "physical_wal_file.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>

using namespace fexma::wal;

namespace {

using Payload = std::array<std::byte, 8>;

class PhysicalControlGuard final {
public:
  explicit PhysicalControlGuard(
      detail::PhysicalWalFileTestControl& control) noexcept {
    detail::set_physical_wal_file_test_control(&control);
  }

  ~PhysicalControlGuard() {
    detail::set_physical_wal_file_test_control(nullptr);
  }

  PhysicalControlGuard(const PhysicalControlGuard&) = delete;
  PhysicalControlGuard& operator=(const PhysicalControlGuard&) = delete;
};

[[nodiscard]] std::filesystem::path test_path(const char* name) {
  return std::filesystem::temp_directory_path() / name;
}

[[nodiscard]] Payload payload(std::uint64_t value) noexcept {
  Payload result{};
  for (std::size_t index = 0; index < result.size(); ++index) {
    result[index] =
        static_cast<std::byte>((value >> (index * 8u)) & 0xffu);
  }
  return result;
}

[[nodiscard]] bool equal(std::span<const std::byte> actual,
                         const Payload& expected) noexcept {
  if (actual.size() != expected.size()) return false;
  for (std::size_t index = 0; index < expected.size(); ++index) {
    if (actual[index] != expected[index]) return false;
  }
  return true;
}

constexpr WalConfig wal_config{static_cast<std::uint32_t>(sizeof(Payload)),
                               8,
                               default_alignment,
                               7,
                               StreamKind::Generic,
                               41,
                               9,
                               100,
                               12};

constexpr PhysicalWalConfig physical_config{
    wal_config.payload_size,           wal_config.alignment,
    wal_config.payload_schema_version, wal_config.stream_kind,
    wal_config.stream_id,              wal_config.epoch_id,
    wal_config.first_sequence,         wal_config.manifest_id};

[[nodiscard]] bool open(RecordTape& tape, PersistenceModule& persistence,
                        const std::filesystem::path& path) {
  return tape.open({wal_config.payload_size, wal_config.capacity,
                   wal_config.alignment})
             .ok() &&
         persistence.open(path, physical_config).ok();
}

[[nodiscard]] bool publish(RecordTape& tape, std::uint64_t value) noexcept {
  const Payload bytes = payload(value);
  return tape.try_publish(std::span<const std::byte>{bytes}).ok();
}

[[nodiscard]] bool batches_sync_then_release_downstream() {
  const auto path = test_path("fexma_wal_persistence_slider_batches.wal");
  std::filesystem::remove(path);

  RecordTape tape;
  PersistenceModule persistence;
  if (!open(tape, persistence, path)) return false;
  for (std::uint64_t value = 0; value < 5; ++value) {
    if (!publish(tape, value)) return false;
  }

  Frontier durable;
  PersistenceSlider persistence_slider(tape, durable, persistence);
  Frontier no_op_frontier;
  NoOpModule no_op;
  Slider no_op_slider(tape, durable, no_op_frontier, no_op);

  detail::PhysicalWalFileTestControl control{};
  PhysicalControlGuard guard(control);

  if (persistence_slider.process_available().status != SliderStatus::Empty ||
      control.append_calls != 0 || control.sync_calls != 0 ||
      no_op_slider.process_available().status != SliderStatus::Empty) {
    return false;
  }

  persistence_slider.set_maximum_count(2);
  const SliderResult first = persistence_slider.process_available();
  if (!first.ok() || first.processed_count != 2 ||
      durable.acquire() != 2 || control.append_calls != 2 ||
      control.sync_calls != 1 || !no_op_slider.process_available().ok() ||
      no_op_frontier.acquire() != 2 ||
      tape.reclaim(no_op_frontier.acquire()) != ReclaimStatus::Ok) {
    return false;
  }

  const SliderResult second = persistence_slider.process_available();
  if (!second.ok() || second.processed_count != 2 ||
      durable.acquire() != 4 || control.append_calls != 4 ||
      control.sync_calls != 2 || !no_op_slider.process_available().ok() ||
      no_op_frontier.acquire() != 4 ||
      tape.reclaim(no_op_frontier.acquire()) != ReclaimStatus::Ok) {
    return false;
  }

  persistence_slider.set_maximum_count(8);
  const SliderResult third = persistence_slider.process_available();
  const SliderResult empty = persistence_slider.process_available();
  if (!third.ok() || third.processed_count != 1 ||
      empty.status != SliderStatus::Empty || durable.acquire() != 5 ||
      control.append_calls != 5 || control.sync_calls != 3 ||
      !no_op_slider.process_available().ok() ||
      no_op_frontier.acquire() != 5 ||
      tape.reclaim(no_op_frontier.acquire()) != ReclaimStatus::Ok ||
      tape.tail() != 5 || tape.head() != 5) {
    return false;
  }

  if (!persistence.close()) return false;
  tape.close();

  WalReader reader;
  if (!reader.open(path, wal_config).ok()) return false;
  Payload output{};
  for (std::uint64_t value = 0; value < 5; ++value) {
    const ReadResult record = reader.read_next(output);
    if (!record.ok() || record.sequence != wal_config.first_sequence + value ||
        !equal(output, payload(value))) {
      return false;
    }
  }
  const bool reader_valid =
      reader.read_next(output).status == ReadStatus::EndOfLog && reader.close();
  const ScanResult scan = scan_wal(path, wal_config);
  const bool valid = reader_valid && scan.status == ScanStatus::Clean &&
                     scan.records == 5 &&
                     scan.last_sequence == wal_config.first_sequence + 4;
  std::filesystem::remove(path);
  return valid;
}

[[nodiscard]] bool append_failure_does_not_publish() {
  const auto path = test_path("fexma_wal_persistence_slider_append_fail.wal");
  std::filesystem::remove(path);

  RecordTape tape;
  PersistenceModule persistence;
  if (!open(tape, persistence, path) || !publish(tape, 0) || !publish(tape, 1)) {
    return false;
  }
  Frontier durable;
  PersistenceSlider slider(tape, durable, persistence, 2);

  detail::PhysicalWalFileTestControl control{};
  control.fail_append_call = 0;
  PhysicalControlGuard guard(control);
  const SliderResult failed = slider.process_available();
  const bool valid = failed.status == SliderStatus::ModuleFailed &&
                     failed.processed_count == 0 && slider.current() == 0 &&
                     durable.acquire() == 0 && persistence.failed() &&
                     control.append_calls == 1 && control.sync_calls == 0;
  (void)persistence.close();
  tape.close();
  std::filesystem::remove(path);
  return valid;
}

[[nodiscard]] bool sync_failure_hides_batch_but_durable_prefix_drains() {
  const auto path = test_path("fexma_wal_persistence_slider_sync_fail.wal");
  std::filesystem::remove(path);

  RecordTape tape;
  PersistenceModule persistence;
  if (!open(tape, persistence, path) || !publish(tape, 0)) return false;

  Frontier durable;
  PersistenceSlider persistence_slider(tape, durable, persistence, 1);
  Frontier no_op_frontier;
  NoOpModule no_op;
  Slider no_op_slider(tape, durable, no_op_frontier, no_op);

  detail::PhysicalWalFileTestControl initial_control{};
  {
    PhysicalControlGuard guard(initial_control);
    if (!persistence_slider.process_available().ok()) return false;
  }
  if (durable.acquire() != 1 || !publish(tape, 1) || !publish(tape, 2)) {
    return false;
  }

  persistence_slider.set_maximum_count(2);
  detail::PhysicalWalFileTestControl failure_control{};
  failure_control.fail_sync_call = 0;
  SliderResult failed{};
  {
    PhysicalControlGuard guard(failure_control);
    failed = persistence_slider.process_available();
  }
  if (failed.status != SliderStatus::PublishFailed ||
      failed.processed_count != 2 || durable.acquire() != 1 ||
      !persistence.failed() || failure_control.append_calls != 2 ||
      failure_control.sync_calls != 1) {
    return false;
  }

  const SliderResult drained = no_op_slider.process_available();
  const SliderResult hidden = no_op_slider.process_available();
  const bool valid = drained.ok() && drained.processed_count == 1 &&
                     hidden.status == SliderStatus::Empty &&
                     no_op_frontier.acquire() == 1 &&
                     tape.reclaim(no_op_frontier.acquire()) ==
                         ReclaimStatus::Ok &&
                     tape.tail() == 1 && tape.head() == 3;
  (void)persistence.close();
  tape.close();
  std::filesystem::remove(path);
  return valid;
}

} // namespace

int main() {
  if (!batches_sync_then_release_downstream()) return 1;
  if (!append_failure_does_not_publish()) return 2;
  if (!sync_failure_hides_batch_but_durable_prefix_drains()) return 3;
  return 0;
}
