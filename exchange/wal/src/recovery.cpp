/**
 * @file recovery.cpp
 * @brief Conservative recovery of an incomplete physical WAL tail.
 */

#include <fexma/wal/recovery.hpp>

#include "physical_wal_adapter.hpp"

#include <system_error>

namespace fexma::wal {
namespace {

[[nodiscard]] bool file_size(const std::filesystem::path& path,
                             std::uint64_t& size) noexcept {
  std::error_code error;
  const std::uintmax_t value = std::filesystem::file_size(path, error);
  if (error) {
    return false;
  }
  size = static_cast<std::uint64_t>(value);
  return static_cast<std::uintmax_t>(size) == value;
}

[[nodiscard]] RecoveryResult result_from_scan(
    RecoveryStatus status, const ScanResult& scan, std::uint64_t original_size,
    std::uint64_t recovered_size) noexcept {
  return {status,
          scan.error,
          scan.records,
          scan.last_sequence,
          scan.last_valid_offset,
          original_size,
          recovered_size,
          original_size >= recovered_size ? original_size - recovered_size
                                          : 0};
}

} // namespace

RecoveryResult recover_incomplete_tail(
    const std::filesystem::path& path,
    const WalConfig& expected) noexcept {
  std::uint64_t original_size{};
  if (!file_size(path, original_size)) {
    return {RecoveryStatus::IoError, WalReadError::IoError};
  }

  const ScanResult before = scan_wal(path, expected);
  switch (before.status) {
  case ScanStatus::Clean:
    return result_from_scan(RecoveryStatus::Clean, before, original_size,
                            original_size);
  case ScanStatus::InvalidConfig:
    return result_from_scan(RecoveryStatus::InvalidConfig, before,
                            original_size, original_size);
  case ScanStatus::Corrupted:
    return result_from_scan(RecoveryStatus::Refused, before, original_size,
                            original_size);
  case ScanStatus::IoError:
    return result_from_scan(RecoveryStatus::IoError, before, original_size,
                            original_size);
  case ScanStatus::IncompleteTail:
    break;
  }

  if (before.last_valid_offset > original_size) {
    return result_from_scan(RecoveryStatus::IoError, before, original_size,
                            original_size);
  }

  detail::PhysicalWalRecoveryAdapter recovery;
  if (!recovery.truncate_and_sync(path, before.last_valid_offset)) {
    std::uint64_t current_size{original_size};
    (void)file_size(path, current_size);
    return result_from_scan(RecoveryStatus::IoError, before, original_size,
                            current_size);
  }

  std::uint64_t recovered_size{};
  if (!file_size(path, recovered_size)) {
    return result_from_scan(RecoveryStatus::IoError, before, original_size,
                            before.last_valid_offset);
  }

  const ScanResult after = scan_wal(path, expected);
  if (!after.ok() || recovered_size != before.last_valid_offset ||
      after.last_valid_offset != before.last_valid_offset ||
      after.records != before.records ||
      after.last_sequence != before.last_sequence) {
    return result_from_scan(RecoveryStatus::IoError, after, original_size,
                            recovered_size);
  }

  RecoveryResult recovered = result_from_scan(
      RecoveryStatus::Recovered, after, original_size, recovered_size);
  recovered.error = before.error;
  return recovered;
}

} // namespace fexma::wal
