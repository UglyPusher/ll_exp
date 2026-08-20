#pragma once

/**
 * @file reader.hpp
 * @brief Validated sequential reader and read-only WAL scanner.
 */

#include <fexma/wal/types.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>

namespace fexma::wal {

namespace detail {
class PhysicalWalReaderAdapter;
}

enum class WalReadError : std::uint8_t {
  None,
  IoError,
  IncompleteFileHeader,
  InvalidFileMagic,
  UnsupportedFormatVersion,
  InvalidFileHeaderSize,
  InvalidFileHeaderCrc,
  InvalidStreamKind,
  UnsupportedFlags,
  IdentityMismatch,
  InvalidPhysicalLayout,
  IncompleteRecordHeader,
  InvalidRecordMagic,
  InvalidRecordHeaderSize,
  InvalidRecordHeaderCrc,
  UnexpectedSequence,
  IncompletePayload,
  InvalidPayloadCrc,
  IncompletePadding,
  NonZeroPadding
};

enum class ReaderOpenStatus : std::uint8_t {
  Ok,
  InvalidConfig,
  Failed,
  AlreadyOpen
};

struct ReaderOpenResult {
  ReaderOpenStatus status{ReaderOpenStatus::Failed};
  WalReadError error{WalReadError::None};

  [[nodiscard]] bool ok() const noexcept {
    return status == ReaderOpenStatus::Ok;
  }
};

enum class ReadStatus : std::uint8_t {
  Record,
  EndOfLog,
  InvalidPayloadSize,
  Failed,
  Closed
};

struct ReadResult {
  ReadStatus status{ReadStatus::Closed};
  WalReadError error{WalReadError::None};
  std::uint64_t sequence{};

  [[nodiscard]] bool ok() const noexcept {
    return status == ReadStatus::Record;
  }
};

class WalReader final {
public:
  WalReader();
  ~WalReader();

  WalReader(const WalReader&) = delete;
  WalReader& operator=(const WalReader&) = delete;
  WalReader(WalReader&&) = delete;
  WalReader& operator=(WalReader&&) = delete;

  [[nodiscard]] ReaderOpenResult
  open(const std::filesystem::path& path,
       const WalConfig& expected) noexcept;
  [[nodiscard]] ReadResult
  read_next(std::span<std::byte> payload) noexcept;
  [[nodiscard]] bool close() noexcept;

  [[nodiscard]] bool is_open() const noexcept;
  [[nodiscard]] std::uint64_t next_sequence() const noexcept;
  [[nodiscard]] std::uint64_t next_offset() const noexcept;

private:
  void fail(WalReadError error) noexcept;

  std::unique_ptr<detail::PhysicalWalReaderAdapter> physical_reader_{};
  WalConfig config_{};
  std::uint64_t next_sequence_{};
  std::uint64_t next_offset_{};
  WalReadError failure_{WalReadError::None};
  bool sequence_exhausted_{};
  bool open_{};
};

enum class ScanStatus : std::uint8_t {
  Clean,
  IncompleteTail,
  Corrupted,
  InvalidConfig,
  IoError
};

struct ScanResult {
  ScanStatus status{ScanStatus::IoError};
  WalReadError error{WalReadError::None};
  std::uint64_t records{};
  std::uint64_t last_sequence{};
  std::uint64_t last_valid_offset{};

  [[nodiscard]] bool ok() const noexcept {
    return status == ScanStatus::Clean;
  }
};

[[nodiscard]] ScanResult
scan_wal(const std::filesystem::path& path,
         const WalConfig& expected) noexcept;

} // namespace fexma::wal
