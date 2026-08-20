#pragma once

/**
 * @file recovery.hpp
 * @brief Conservative mutation policy for an incomplete physical WAL tail.
 */

#include <fexma/wal/reader.hpp>

#include <cstdint>
#include <filesystem>

namespace fexma::wal {

enum class RecoveryStatus : std::uint8_t {
  Clean,
  Recovered,
  Refused,
  InvalidConfig,
  IoError
};

struct RecoveryResult {
  RecoveryStatus status{RecoveryStatus::IoError};
  WalReadError error{WalReadError::None};
  std::uint64_t records{};
  std::uint64_t last_sequence{};
  std::uint64_t last_valid_offset{};
  std::uint64_t original_size{};
  std::uint64_t recovered_size{};
  std::uint64_t removed_bytes{};

  [[nodiscard]] bool ok() const noexcept {
    return status == RecoveryStatus::Clean ||
           status == RecoveryStatus::Recovered;
  }
};

/**
 * @brief Removes only a scanner-proven incomplete physical tail.
 *
 * The caller owns exclusive access to the quiescent file for the complete
 * operation. Corruption, identity mismatch, and complete invalid records are
 * refused without mutation. Successful truncation is physically synchronized
 * and followed by a complete validated rescan.
 */
[[nodiscard]] RecoveryResult recover_incomplete_tail(
    const std::filesystem::path& path,
    const WalConfig& expected) noexcept;

} // namespace fexma::wal
