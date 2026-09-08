/**
 * @file test_wal_persistence_slider.cpp
 * @brief Contract tests for persistence attached through the generic slider.
 */

#include <fexma/wal/noop_module.hpp>
#include <fexma/wal/persistence_slider.hpp>
#include <fexma/wal/reader.hpp>
#include <fexma/wal/wal.hpp>

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

[[nodiscard]] bool open(WalCore& wal, PersistenceModule& persistence,
                        const std::filesystem::path& path) {
  return wal.open({wal_config.payload_size, wal_config.capacity,
                   wal_config.alignment, wal_config.first_sequence})
             .ok() &&
         persistence.open(path, physical_config).ok();
}

[[nodiscard]] bool publish(WalCore& wal, std::uint64_t value) noexcept {
  const Payload bytes = payload(value);
  return wal.try_publish(std::span<const std::byte>{bytes}).ok();
}

[[nodiscard]] bool publish(Wal& wal, std::uint64_t value) noexcept {
  const Payload bytes = payload(value);
  return wal.try_publish(std::span<const std::byte>{bytes}).ok();
}

[[nodiscard]] bool batches_sync_then_release_downstream() {
  const auto path = test_path("fexma_wal_persistence_slider_batches.wal");
  std::filesystem::remove(path);

  WalCore wal;
  PersistenceModule persistence;
  if (!open(wal, persistence, path)) return false;
  for (std::uint64_t value = 0; value < 5; ++value) {
    if (!publish(wal, value)) return false;
  }

  WalHeadProgress head(wal);
  Progress durable;
  PersistenceSlider persistence_slider(
      wal, head, durable.writer(), persistence, 0, BoundedRangeAcquire{},
      PersistenceBatchPublish{});
  Progress no_op_frontier;
  NoOpModule no_op;
  Slider no_op_slider(wal, durable.reader(), no_op_frontier.writer(), no_op);

  detail::PhysicalWalFileTestControl control{};
  PhysicalControlGuard guard(control);

  if (persistence_slider.process_available().status != SliderStatus::Empty ||
      control.append_calls != 0 || control.sync_calls != 0 ||
      no_op_slider.process_available().status != SliderStatus::Empty) {
    return false;
  }

  persistence_slider.acquire_policy().set_maximum_count(2);
  const SliderResult first = persistence_slider.process_available();
  if (!first.ok() || first.processed_count != 2 ||
      durable.reader().acquire() != 2 || control.append_calls != 2 ||
      control.sync_calls != 1 || !no_op_slider.process_available().ok() ||
      no_op_frontier.reader().acquire() != 2 ||
      wal.reclaim(no_op_frontier.reader().acquire()) != ReclaimStatus::Ok) {
    return false;
  }

  const SliderResult second = persistence_slider.process_available();
  if (!second.ok() || second.processed_count != 2 ||
      durable.reader().acquire() != 4 || control.append_calls != 4 ||
      control.sync_calls != 2 || !no_op_slider.process_available().ok() ||
      no_op_frontier.reader().acquire() != 4 ||
      wal.reclaim(no_op_frontier.reader().acquire()) != ReclaimStatus::Ok) {
    return false;
  }

  persistence_slider.acquire_policy().set_maximum_count(8);
  const SliderResult third = persistence_slider.process_available();
  const SliderResult empty = persistence_slider.process_available();
  if (!third.ok() || third.processed_count != 1 ||
      empty.status != SliderStatus::Empty || durable.reader().acquire() != 5 ||
      control.append_calls != 5 || control.sync_calls != 3 ||
      !no_op_slider.process_available().ok() ||
      no_op_frontier.reader().acquire() != 5 ||
      wal.reclaim(no_op_frontier.reader().acquire()) != ReclaimStatus::Ok ||
      wal.tail() != 5 || wal.head() != 5) {
    return false;
  }

  if (!persistence.close()) return false;
  wal.close();

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

  WalCore wal;
  PersistenceModule persistence;
  if (!open(wal, persistence, path) || !publish(wal, 0) || !publish(wal, 1)) {
    return false;
  }
  WalHeadProgress head(wal);
  Progress durable;
  PersistenceSlider slider(wal, head, durable.writer(), persistence, 0,
                           BoundedRangeAcquire{2},
                           PersistenceBatchPublish{});

  detail::PhysicalWalFileTestControl control{};
  control.fail_append_call = 0;
  PhysicalControlGuard guard(control);
  const SliderResult failed = slider.process_available();
  const bool valid = failed.status == SliderStatus::ModuleFailed &&
                     failed.processed_count == 0 && slider.current() == 0 &&
                     durable.reader().acquire() == 0 && persistence.failed() &&
                     control.append_calls == 1 && control.sync_calls == 0;
  (void)persistence.close();
  wal.close();
  std::filesystem::remove(path);
  return valid;
}

[[nodiscard]] bool sync_failure_hides_batch_but_durable_prefix_drains() {
  const auto path = test_path("fexma_wal_persistence_slider_sync_fail.wal");
  std::filesystem::remove(path);

  WalCore wal;
  PersistenceModule persistence;
  if (!open(wal, persistence, path) || !publish(wal, 0)) return false;

  WalHeadProgress head(wal);
  Progress durable;
  PersistenceSlider persistence_slider(
      wal, head, durable.writer(), persistence, 0, BoundedRangeAcquire{1},
      PersistenceBatchPublish{});
  Progress no_op_frontier;
  NoOpModule no_op;
  Slider no_op_slider(wal, durable.reader(), no_op_frontier.writer(), no_op);

  detail::PhysicalWalFileTestControl initial_control{};
  {
    PhysicalControlGuard guard(initial_control);
    if (!persistence_slider.process_available().ok()) return false;
  }
  if (durable.reader().acquire() != 1 || !publish(wal, 1) || !publish(wal, 2)) {
    return false;
  }

  persistence_slider.acquire_policy().set_maximum_count(2);
  detail::PhysicalWalFileTestControl failure_control{};
  failure_control.fail_sync_call = 0;
  SliderResult failed{};
  {
    PhysicalControlGuard guard(failure_control);
    failed = persistence_slider.process_available();
  }
  if (failed.status != SliderStatus::PublishFailed ||
      failed.processed_count != 2 || durable.reader().acquire() != 1 ||
      !persistence.failed() || failure_control.append_calls != 2 ||
      failure_control.sync_calls != 1) {
    return false;
  }

  const SliderResult drained = no_op_slider.process_available();
  const SliderResult hidden = no_op_slider.process_available();
  const bool valid = drained.ok() && drained.processed_count == 1 &&
                     hidden.status == SliderStatus::Empty &&
                     no_op_frontier.reader().acquire() == 1 &&
                     wal.reclaim(no_op_frontier.reader().acquire()) ==
                         ReclaimStatus::Ok &&
                     wal.tail() == 1 && wal.head() == 3;
  (void)persistence.close();
  wal.close();
  std::filesystem::remove(path);
  return valid;
}

[[nodiscard]] bool compatibility_composition_reopens_with_fresh_progress() {
  const auto first_path = test_path("fexma_wal_slider_reopen_first.wal");
  const auto second_path = test_path("fexma_wal_slider_reopen_second.wal");
  std::filesystem::remove(first_path);
  std::filesystem::remove(second_path);

  Wal wal;
  Payload output{};
  if (!wal.open(first_path, wal_config).ok() || !publish(wal, 0) ||
      !wal.advance_durable().ok() || !wal.try_consume(output).ok() ||
      !equal(output, payload(0)) || !wal.close().ok()) {
    return false;
  }

  const PublishResult closed_publish = wal.try_publish(payload(1));
  if (closed_publish.status != PublishStatus::Closed ||
      !wal.open(second_path, wal_config).ok() ||
      wal.snapshot().tail != 0 || wal.snapshot().durable != 0 ||
      wal.snapshot().head != 0 || !publish(wal, 0) ||
      !wal.advance_durable().ok() || !wal.try_consume(output).ok() ||
      !equal(output, payload(0)) || !wal.close().ok()) {
    return false;
  }

  std::filesystem::remove(first_path);
  std::filesystem::remove(second_path);
  return true;
}

} // namespace

int main() {
  if (!batches_sync_then_release_downstream()) return 1;
  if (!append_failure_does_not_publish()) return 2;
  if (!sync_failure_hides_batch_but_durable_prefix_drains()) return 3;
  if (!compatibility_composition_reopens_with_fresh_progress()) return 4;
  return 0;
}
