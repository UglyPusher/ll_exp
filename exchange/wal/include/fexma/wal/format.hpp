#pragma once

/**
 * @file format.hpp
 * @brief Fixed raw WAL file and record headers.
 *
 * These headers describe physical storage only. They do not describe command,
 * event, instrument, or business payload semantics.
 */

#include <fexma/wal/types.hpp>

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace fexma::wal {

struct FileHeader {
  std::uint32_t magic{file_magic};
  std::uint16_t version{format_version};
  std::uint16_t header_size{sizeof(FileHeader)};
  std::uint32_t payload_size{};
  std::uint32_t alignment{default_alignment};
  std::uint64_t next_sequence{1};
  std::uint32_t header_crc32{};
  std::uint32_t reserved{};
};

struct RecordHeader {
  std::uint32_t magic{record_magic};
  std::uint16_t version{format_version};
  std::uint16_t header_size{sizeof(RecordHeader)};
  std::uint64_t sequence{};
  std::uint32_t payload_crc32{};
  std::uint32_t header_crc32{};
};

static_assert(std::is_trivially_copyable_v<FileHeader>);
static_assert(std::is_standard_layout_v<FileHeader>);
static_assert(std::is_trivially_copyable_v<RecordHeader>);
static_assert(std::is_standard_layout_v<RecordHeader>);

[[nodiscard]] std::uint32_t crc32_bytes(const void* data,
                                        std::size_t size) noexcept;
[[nodiscard]] std::uint32_t file_header_crc32(FileHeader header) noexcept;
[[nodiscard]] std::uint32_t record_header_crc32(RecordHeader header) noexcept;
[[nodiscard]] std::uint64_t aligned_record_size(const WalConfig& config) noexcept;
[[nodiscard]] bool valid_config(const WalConfig& config) noexcept;

} // namespace fexma::wal
