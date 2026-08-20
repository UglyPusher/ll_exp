/**
 * @file test_wal_recovery.cpp
 * @brief Contract tests for conservative incomplete-tail recovery.
 */

#include <fexma/wal/format.hpp>
#include <fexma/wal/recovery.hpp>
#include <fexma/wal/wal.hpp>

#include "physical_wal_file.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <vector>

using namespace fexma::wal;

namespace {

constexpr WalConfig config{8, 8, 64, 11, StreamKind::Command,
                           71, 5, 101, 29};

class RecoveryControlGuard final {
public:
  explicit RecoveryControlGuard(
      detail::PhysicalWalRecoveryTestControl& control) noexcept {
    detail::set_physical_wal_recovery_test_control(&control);
  }

  ~RecoveryControlGuard() {
    detail::set_physical_wal_recovery_test_control(nullptr);
  }

  RecoveryControlGuard(const RecoveryControlGuard&) = delete;
  RecoveryControlGuard& operator=(const RecoveryControlGuard&) = delete;
};

[[nodiscard]] std::filesystem::path test_path(const char* name) {
  return std::filesystem::temp_directory_path() / name;
}

[[nodiscard]] std::array<std::byte, config.payload_size>
payload(std::uint64_t value) noexcept {
  std::array<std::byte, config.payload_size> bytes{};
  for (std::size_t index = 0; index < bytes.size(); ++index) {
    bytes[index] = static_cast<std::byte>((value + index * 23u) & 0xffu);
  }
  return bytes;
}

[[nodiscard]] bool create_wal(const std::filesystem::path& path,
                              std::uint32_t records) {
  std::filesystem::remove(path);
  Wal wal;
  if (!wal.open(path, config).ok()) {
    return false;
  }
  for (std::uint32_t index = 0; index < records; ++index) {
    if (!wal.try_publish(payload(index + 1u)).ok()) {
      return false;
    }
  }
  if (!wal.advance_durable().ok()) {
    return false;
  }
  std::array<std::byte, config.payload_size> out{};
  for (std::uint32_t index = 0; index < records; ++index) {
    if (!wal.try_consume(out).ok()) {
      return false;
    }
  }
  return wal.close().ok();
}

[[nodiscard]] std::vector<std::byte>
read_file(const std::filesystem::path& path) {
  const auto size = static_cast<std::size_t>(std::filesystem::file_size(path));
  std::vector<std::byte> bytes(size);
  std::ifstream file(path, std::ios::binary);
  file.read(reinterpret_cast<char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
  return bytes;
}

void overwrite_byte(const std::filesystem::path& path, std::uint64_t offset,
                    std::byte value) {
  std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
  file.seekp(static_cast<std::streamoff>(offset));
  file.write(reinterpret_cast<const char*>(&value), 1);
}

[[nodiscard]] bool clean_wal_is_not_modified() {
  const auto path = test_path("fexma_wal_recovery_clean.wal");
  if (!create_wal(path, 2)) {
    return false;
  }
  const auto before = read_file(path);
  const RecoveryResult recovered = recover_incomplete_tail(path, config);
  const auto after = read_file(path);
  std::filesystem::remove(path);
  return recovered.status == RecoveryStatus::Clean &&
         recovered.original_size == before.size() &&
         recovered.recovered_size == before.size() &&
         recovered.removed_bytes == 0 && before == after;
}

[[nodiscard]] bool recover_truncated_second_record(
    const char* name, std::uint64_t retained_second_record_bytes,
    WalReadError expected_error) {
  const auto path = test_path(name);
  if (!create_wal(path, 2)) {
    return false;
  }

  const std::uint64_t first_end =
      records_offset(config) + aligned_record_size(config);
  const std::uint64_t truncated_size =
      first_end + retained_second_record_bytes;
  std::filesystem::resize_file(path, truncated_size);

  const RecoveryResult recovered = recover_incomplete_tail(path, config);
  const ScanResult scan = scan_wal(path, config);
  const RecoveryResult repeated = recover_incomplete_tail(path, config);
  const std::uint64_t final_size = std::filesystem::file_size(path);
  std::filesystem::remove(path);

  return recovered.status == RecoveryStatus::Recovered &&
         recovered.error == expected_error && recovered.records == 1 &&
         recovered.last_sequence == config.first_sequence &&
         recovered.last_valid_offset == first_end &&
         recovered.original_size == truncated_size &&
         recovered.recovered_size == first_end &&
         recovered.removed_bytes == retained_second_record_bytes &&
         final_size == first_end && scan.ok() && scan.records == 1 &&
         repeated.status == RecoveryStatus::Clean;
}

[[nodiscard]] bool incomplete_tails_are_recovered() {
  const auto first_path = test_path("fexma_wal_recovery_partial_first.wal");
  if (!create_wal(first_path, 1)) {
    return false;
  }
  const std::uint64_t first_partial_size = records_offset(config) + 7u;
  std::filesystem::resize_file(first_path, first_partial_size);
  const RecoveryResult first_recovered =
      recover_incomplete_tail(first_path, config);
  const std::uint64_t first_final_size =
      std::filesystem::file_size(first_path);
  std::filesystem::remove(first_path);
  if (first_recovered.status != RecoveryStatus::Recovered ||
      first_recovered.error != WalReadError::IncompleteRecordHeader ||
      first_recovered.records != 0 || first_recovered.last_sequence != 0 ||
      first_recovered.last_valid_offset != records_offset(config) ||
      first_final_size != records_offset(config)) {
    return false;
  }

  return recover_truncated_second_record(
             "fexma_wal_recovery_partial_header.wal", 7,
             WalReadError::IncompleteRecordHeader) &&
         recover_truncated_second_record(
             "fexma_wal_recovery_partial_payload.wal",
             physical_record_header_size + 3u,
             WalReadError::IncompletePayload) &&
         recover_truncated_second_record(
             "fexma_wal_recovery_partial_padding.wal",
             physical_record_header_size + config.payload_size + 3u,
             WalReadError::IncompletePadding);
}

[[nodiscard]] bool corruption_and_identity_mismatch_are_refused() {
  const auto path = test_path("fexma_wal_recovery_refused.wal");
  if (!create_wal(path, 2)) {
    return false;
  }
  overwrite_byte(path, records_offset(config) + physical_record_header_size,
                 std::byte{0xee});
  const auto corrupted = read_file(path);
  const RecoveryResult refused = recover_incomplete_tail(path, config);
  const auto after_refusal = read_file(path);
  if (refused.status != RecoveryStatus::Refused ||
      refused.error != WalReadError::InvalidPayloadCrc ||
      corrupted != after_refusal) {
    return false;
  }

  WalConfig wrong = config;
  ++wrong.epoch_id;
  const RecoveryResult mismatch = recover_incomplete_tail(path, wrong);
  const auto after_mismatch = read_file(path);
  std::filesystem::remove(path);
  if (mismatch.status != RecoveryStatus::Refused ||
      mismatch.error != WalReadError::IdentityMismatch ||
      corrupted != after_mismatch) {
    return false;
  }

  const auto last_path =
      test_path("fexma_wal_recovery_invalid_last_record.wal");
  if (!create_wal(last_path, 1)) {
    return false;
  }
  overwrite_byte(last_path, records_offset(config) + 20u, std::byte{0xff});
  const auto invalid_last = read_file(last_path);
  const RecoveryResult last_refused =
      recover_incomplete_tail(last_path, config);
  const auto last_after = read_file(last_path);
  std::filesystem::remove(last_path);
  return last_refused.status == RecoveryStatus::Refused &&
         last_refused.error == WalReadError::InvalidRecordHeaderCrc &&
         invalid_last == last_after;
}

[[nodiscard]] bool incomplete_file_header_is_refused() {
  const auto path = test_path("fexma_wal_recovery_partial_file_header.wal");
  if (!create_wal(path, 1)) {
    return false;
  }
  std::filesystem::resize_file(path, 17);
  const auto before = read_file(path);
  const RecoveryResult refused = recover_incomplete_tail(path, config);
  const auto after = read_file(path);
  std::filesystem::remove(path);
  return refused.status == RecoveryStatus::Refused &&
         refused.error == WalReadError::IncompleteFileHeader &&
         before == after;
}

[[nodiscard]] bool truncate_and_sync_failures_are_reported() {
  const auto truncate_path = test_path("fexma_wal_recovery_truncate_fail.wal");
  if (!create_wal(truncate_path, 2)) {
    return false;
  }
  const std::uint64_t first_end =
      records_offset(config) + aligned_record_size(config);
  std::filesystem::resize_file(truncate_path, first_end + 7u);
  detail::PhysicalWalRecoveryTestControl truncate_control{};
  truncate_control.fail_truncate_call = 0;
  RecoveryResult truncate_failed{};
  {
    RecoveryControlGuard guard(truncate_control);
    truncate_failed = recover_incomplete_tail(truncate_path, config);
  }
  const std::uint64_t size_after_truncate_failure =
      std::filesystem::file_size(truncate_path);
  std::filesystem::remove(truncate_path);
  if (truncate_failed.status != RecoveryStatus::IoError ||
      truncate_control.truncate_calls != 1 ||
      truncate_control.sync_calls != 0 ||
      size_after_truncate_failure != first_end + 7u) {
    return false;
  }

  const auto sync_path = test_path("fexma_wal_recovery_sync_fail.wal");
  if (!create_wal(sync_path, 2)) {
    return false;
  }
  std::filesystem::resize_file(sync_path, first_end + 7u);
  detail::PhysicalWalRecoveryTestControl sync_control{};
  sync_control.fail_sync_call = 0;
  RecoveryResult sync_failed{};
  {
    RecoveryControlGuard guard(sync_control);
    sync_failed = recover_incomplete_tail(sync_path, config);
  }
  const std::uint64_t size_after_sync_failure =
      std::filesystem::file_size(sync_path);
  std::filesystem::remove(sync_path);
  return sync_failed.status == RecoveryStatus::IoError &&
         sync_control.truncate_calls == 1 && sync_control.sync_calls == 1 &&
         sync_failed.original_size == first_end + 7u &&
         sync_failed.recovered_size == first_end &&
         size_after_sync_failure == first_end;
}

} // namespace

int main() {
  if (!clean_wal_is_not_modified()) return 1;
  if (!incomplete_tails_are_recovered()) return 2;
  if (!corruption_and_identity_mismatch_are_refused()) return 3;
  if (!incomplete_file_header_is_refused()) return 4;
  if (!truncate_and_sync_failures_are_reported()) return 5;
  return 0;
}
