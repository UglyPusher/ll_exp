/**
 * @file reader.cpp
 * @brief Validated sequential reader and read-only WAL scanner.
 */

#include <fexma/wal/format.hpp>
#include <fexma/wal/reader.hpp>

#include "physical_wal_adapter.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <new>

namespace fexma::wal {
namespace {

[[nodiscard]] bool known_stream_kind(StreamKind kind) noexcept {
  switch (kind) {
  case StreamKind::Generic:
  case StreamKind::Command:
  case StreamKind::Event:
    return true;
  }
  return false;
}

[[nodiscard]] bool same_persisted_config(const FileHeader& header,
                                         const WalConfig& expected) noexcept {
  return header.stream_kind == expected.stream_kind &&
         header.payload_size == expected.payload_size &&
         header.payload_schema_version == expected.payload_schema_version &&
         header.alignment == expected.alignment &&
         header.stream_id == expected.stream_id &&
         header.epoch_id == expected.epoch_id &&
         header.first_sequence == expected.first_sequence &&
         header.manifest_id == expected.manifest_id;
}

[[nodiscard]] bool valid_reader_config(const WalConfig& expected) noexcept {
  WalConfig validation = expected;
  validation.capacity = 1;
  return valid_config(validation);
}

[[nodiscard]] WalReadError read_zero_padding(
    detail::PhysicalWalReaderAdapter& reader, std::uint64_t size,
    WalReadError incomplete_error) noexcept {
  std::array<std::byte, 4096> buffer{};
  while (size != 0) {
    const std::size_t chunk = static_cast<std::size_t>(
        std::min<std::uint64_t>(size, buffer.size()));
    const detail::PhysicalReadStatus status =
        reader.read({buffer.data(), chunk});
    if (status == detail::PhysicalReadStatus::IoError) {
      return WalReadError::IoError;
    }
    if (status != detail::PhysicalReadStatus::Complete) {
      return incomplete_error;
    }
    if (!std::all_of(buffer.begin(), buffer.begin() + chunk,
                     [](std::byte value) { return value == std::byte{0}; })) {
      return WalReadError::NonZeroPadding;
    }
    size -= chunk;
  }
  return WalReadError::None;
}

[[nodiscard]] ScanStatus scan_status_for(WalReadError error) noexcept {
  switch (error) {
  case WalReadError::IncompleteRecordHeader:
  case WalReadError::IncompletePayload:
  case WalReadError::IncompletePadding:
    return ScanStatus::IncompleteTail;
  case WalReadError::IoError:
    return ScanStatus::IoError;
  default:
    return ScanStatus::Corrupted;
  }
}

} // namespace

WalReader::WalReader() = default;

WalReader::~WalReader() { (void)close(); }

ReaderOpenResult WalReader::open(const std::filesystem::path& path,
                                 const WalConfig& expected) noexcept {
  if (open_) {
    return {ReaderOpenStatus::AlreadyOpen, WalReadError::None};
  }
  if (!valid_reader_config(expected)) {
    return {ReaderOpenStatus::InvalidConfig, WalReadError::None};
  }

  physical_reader_.reset(
      new (std::nothrow) detail::PhysicalWalReaderAdapter{});
  if (!physical_reader_ || !physical_reader_->open(path)) {
    physical_reader_.reset();
    return {ReaderOpenStatus::Failed, WalReadError::IoError};
  }

  std::array<std::byte, physical_file_header_size> bytes{};
  const detail::PhysicalReadStatus read_status = physical_reader_->read(bytes);
  if (read_status != detail::PhysicalReadStatus::Complete) {
    const WalReadError error =
        read_status == detail::PhysicalReadStatus::IoError
            ? WalReadError::IoError
            : WalReadError::IncompleteFileHeader;
    (void)close();
    return {ReaderOpenStatus::Failed, error};
  }

  FileHeader header{};
  (void)deserialize_file_header(bytes, header);
  WalReadError error{WalReadError::None};
  if (header.magic != file_magic) {
    error = WalReadError::InvalidFileMagic;
  } else if (header.version != format_version) {
    error = WalReadError::UnsupportedFormatVersion;
  } else if (header.header_size != physical_file_header_size) {
    error = WalReadError::InvalidFileHeaderSize;
  } else if (file_header_crc32(header) != header.header_crc32) {
    error = WalReadError::InvalidFileHeaderCrc;
  } else if (!known_stream_kind(header.stream_kind)) {
    error = WalReadError::InvalidStreamKind;
  } else if (header.flags != 0) {
    error = WalReadError::UnsupportedFlags;
  } else {
    WalConfig actual{header.payload_size,
                     1,
                     header.alignment,
                     header.payload_schema_version,
                     header.stream_kind,
                     header.stream_id,
                     header.epoch_id,
                     header.first_sequence,
                     header.manifest_id};
    if (!valid_config(actual) ||
        header.records_offset != records_offset(actual)) {
      error = WalReadError::InvalidPhysicalLayout;
    } else if (!same_persisted_config(header, expected)) {
      error = WalReadError::IdentityMismatch;
    }
  }

  if (error != WalReadError::None) {
    (void)close();
    return {ReaderOpenStatus::Failed, error};
  }

  const std::uint64_t header_padding =
      header.records_offset - physical_file_header_size;
  error = read_zero_padding(*physical_reader_, header_padding,
                            WalReadError::IncompleteFileHeader);
  if (error != WalReadError::None) {
    if (error == WalReadError::NonZeroPadding) {
      error = WalReadError::InvalidPhysicalLayout;
    }
    (void)close();
    return {ReaderOpenStatus::Failed, error};
  }

  config_ = expected;
  next_sequence_ = expected.first_sequence;
  next_offset_ = header.records_offset;
  failure_ = WalReadError::None;
  sequence_exhausted_ = false;
  open_ = true;
  return {ReaderOpenStatus::Ok, WalReadError::None};
}

ReadResult WalReader::read_next(std::span<std::byte> payload) noexcept {
  if (!open_) {
    return {ReadStatus::Closed, WalReadError::None, 0};
  }
  if (failure_ != WalReadError::None) {
    return {ReadStatus::Failed, failure_, 0};
  }
  if (payload.size() != config_.payload_size) {
    return {ReadStatus::InvalidPayloadSize, WalReadError::None, 0};
  }

  std::array<std::byte, physical_record_header_size> header_bytes{};
  const detail::PhysicalReadStatus header_status =
      physical_reader_->read(header_bytes);
  if (header_status == detail::PhysicalReadStatus::EndOfFile) {
    return {ReadStatus::EndOfLog, WalReadError::None, 0};
  }
  if (header_status != detail::PhysicalReadStatus::Complete) {
    const WalReadError error =
        header_status == detail::PhysicalReadStatus::IoError
            ? WalReadError::IoError
            : WalReadError::IncompleteRecordHeader;
    fail(error);
    return {ReadStatus::Failed, error, 0};
  }
  if (sequence_exhausted_) {
    fail(WalReadError::UnexpectedSequence);
    return {ReadStatus::Failed, WalReadError::UnexpectedSequence, 0};
  }

  RecordHeader header{};
  (void)deserialize_record_header(header_bytes, header);
  WalReadError error{WalReadError::None};
  if (header.magic != record_magic) {
    error = WalReadError::InvalidRecordMagic;
  } else if (header.version != format_version) {
    error = WalReadError::UnsupportedFormatVersion;
  } else if (header.header_size != physical_record_header_size) {
    error = WalReadError::InvalidRecordHeaderSize;
  } else if (record_header_crc32(header) != header.header_crc32) {
    error = WalReadError::InvalidRecordHeaderCrc;
  } else if (header.sequence != next_sequence_) {
    error = WalReadError::UnexpectedSequence;
  }
  if (error != WalReadError::None) {
    fail(error);
    return {ReadStatus::Failed, error, 0};
  }

  const detail::PhysicalReadStatus payload_status =
      physical_reader_->read(payload);
  if (payload_status != detail::PhysicalReadStatus::Complete) {
    error = payload_status == detail::PhysicalReadStatus::IoError
                ? WalReadError::IoError
                : WalReadError::IncompletePayload;
    fail(error);
    return {ReadStatus::Failed, error, 0};
  }
  if (crc32_bytes(payload.data(), payload.size()) != header.payload_crc32) {
    error = WalReadError::InvalidPayloadCrc;
    fail(error);
    return {ReadStatus::Failed, error, 0};
  }

  const std::uint64_t padding = aligned_record_size(config_) -
                                physical_record_header_size -
                                config_.payload_size;
  error = read_zero_padding(*physical_reader_, padding,
                            WalReadError::IncompletePadding);
  if (error != WalReadError::None) {
    fail(error);
    return {ReadStatus::Failed, error, 0};
  }

  const std::uint64_t sequence = next_sequence_;
  if (next_sequence_ == std::numeric_limits<std::uint64_t>::max()) {
    sequence_exhausted_ = true;
  } else {
    ++next_sequence_;
  }
  next_offset_ += aligned_record_size(config_);
  return {ReadStatus::Record, WalReadError::None, sequence};
}

bool WalReader::close() noexcept {
  const bool closed = !physical_reader_ || physical_reader_->close();
  physical_reader_.reset();
  config_ = {};
  next_sequence_ = 0;
  next_offset_ = 0;
  failure_ = WalReadError::None;
  sequence_exhausted_ = false;
  open_ = false;
  return closed;
}

bool WalReader::is_open() const noexcept { return open_; }

std::uint64_t WalReader::next_sequence() const noexcept {
  return next_sequence_;
}

std::uint64_t WalReader::next_offset() const noexcept { return next_offset_; }

void WalReader::fail(WalReadError error) noexcept { failure_ = error; }

ScanResult scan_wal(const std::filesystem::path& path,
                    const WalConfig& expected) noexcept {
  WalReader reader;
  const ReaderOpenResult opened = reader.open(path, expected);
  if (!opened.ok()) {
    return {opened.status == ReaderOpenStatus::InvalidConfig
                ? ScanStatus::InvalidConfig
                : (opened.error == WalReadError::IoError
                       ? ScanStatus::IoError
                       : ScanStatus::Corrupted),
            opened.error, 0, 0, 0};
  }

  std::unique_ptr<std::byte[]> payload{
      new (std::nothrow) std::byte[expected.payload_size]};
  if (!payload) {
    return {ScanStatus::IoError, WalReadError::IoError, 0, 0,
            reader.next_offset()};
  }

  ScanResult result{ScanStatus::Clean, WalReadError::None, 0, 0,
                    reader.next_offset()};
  while (true) {
    const ReadResult read =
        reader.read_next({payload.get(), expected.payload_size});
    if (read.status == ReadStatus::Record) {
      ++result.records;
      result.last_sequence = read.sequence;
      result.last_valid_offset = reader.next_offset();
      continue;
    }
    if (read.status == ReadStatus::EndOfLog) {
      (void)reader.close();
      return result;
    }
    result.status = scan_status_for(read.error);
    result.error = read.error;
    (void)reader.close();
    return result;
  }
}

} // namespace fexma::wal
