#pragma once

/**
 * @file format.hpp
 * @brief Fixed raw WAL file and record headers.
 *
 * These headers describe physical storage only. They do not describe command,
 * event, instrument, or business payload semantics.
 */

#include <fexma/wal/types.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>

namespace fexma::wal {

inline constexpr std::uint16_t physical_file_header_size = 64;
inline constexpr std::uint16_t physical_record_header_size = 24;

struct FileHeader {
  std::uint32_t magic{file_magic};
  std::uint16_t version{format_version};
  std::uint16_t header_size{physical_file_header_size};
  StreamKind stream_kind{StreamKind::Generic};
  std::uint16_t flags{};
  std::uint32_t payload_size{};
  std::uint32_t payload_schema_version{};
  std::uint32_t alignment{default_alignment};
  std::uint32_t records_offset{};
  StreamId stream_id{};
  EpochId epoch_id{};
  std::uint64_t first_sequence{1};
  ManifestId manifest_id{};
  std::uint32_t header_crc32{};
};

struct RecordHeader {
  std::uint32_t magic{record_magic};
  std::uint16_t version{format_version};
  std::uint16_t header_size{physical_record_header_size};
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
[[nodiscard]] std::array<std::byte, physical_file_header_size>
serialize_file_header(FileHeader header) noexcept;
[[nodiscard]] std::array<std::byte, physical_record_header_size>
serialize_record_header(RecordHeader header) noexcept;
[[nodiscard]] bool deserialize_file_header(
    std::span<const std::byte> bytes, FileHeader& header) noexcept;
[[nodiscard]] bool deserialize_record_header(
    std::span<const std::byte> bytes, RecordHeader& header) noexcept;
[[nodiscard]] std::uint32_t file_header_crc32(FileHeader header) noexcept;
[[nodiscard]] std::uint32_t record_header_crc32(RecordHeader header) noexcept;
[[nodiscard]] std::uint64_t aligned_record_size(const WalConfig& config) noexcept;
[[nodiscard]] std::uint32_t records_offset(const WalConfig& config) noexcept;
[[nodiscard]] bool valid_config(const WalConfig& config) noexcept;

} // namespace fexma::wal
