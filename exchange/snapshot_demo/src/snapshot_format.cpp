#include <fexma/snapshot_demo/snapshot_format.hpp>

#include <fexma/wal/format.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace fexma::snapshot_demo {
namespace {

constexpr std::uint32_t hash_snapshot_magic = 0x36485346u; // "FSH6"
constexpr std::uint32_t bit_snapshot_magic = 0x36425346u;  // "FSB6"
constexpr std::uint32_t description_magic = 0x36445346u;   // "FSD6"
constexpr std::uint32_t required_module_count = 2u;

void put_u16(std::span<std::byte> out, std::size_t offset,
             std::uint16_t value) noexcept {
  out[offset] = static_cast<std::byte>(value & 0xffu);
  out[offset + 1u] = static_cast<std::byte>((value >> 8u) & 0xffu);
}

void put_u32(std::span<std::byte> out, std::size_t offset,
             std::uint32_t value) noexcept {
  out[offset] = static_cast<std::byte>(value & 0xffu);
  out[offset + 1u] = static_cast<std::byte>((value >> 8u) & 0xffu);
  out[offset + 2u] = static_cast<std::byte>((value >> 16u) & 0xffu);
  out[offset + 3u] = static_cast<std::byte>((value >> 24u) & 0xffu);
}

void put_u64(std::span<std::byte> out, std::size_t offset,
             std::uint64_t value) noexcept {
  out[offset] = static_cast<std::byte>(value & 0xffu);
  out[offset + 1u] = static_cast<std::byte>((value >> 8u) & 0xffu);
  out[offset + 2u] = static_cast<std::byte>((value >> 16u) & 0xffu);
  out[offset + 3u] = static_cast<std::byte>((value >> 24u) & 0xffu);
  out[offset + 4u] = static_cast<std::byte>((value >> 32u) & 0xffu);
  out[offset + 5u] = static_cast<std::byte>((value >> 40u) & 0xffu);
  out[offset + 6u] = static_cast<std::byte>((value >> 48u) & 0xffu);
  out[offset + 7u] = static_cast<std::byte>((value >> 56u) & 0xffu);
}

[[nodiscard]] std::uint16_t get_u16(std::span<const std::byte> bytes,
                                    std::size_t offset) noexcept {
  return static_cast<std::uint16_t>(
      std::to_integer<std::uint8_t>(bytes[offset]) |
      (static_cast<std::uint16_t>(
           std::to_integer<std::uint8_t>(bytes[offset + 1u]))
       << 8u));
}

[[nodiscard]] std::uint32_t get_u32(std::span<const std::byte> bytes,
                                    std::size_t offset) noexcept {
  return static_cast<std::uint32_t>(
      std::to_integer<std::uint8_t>(bytes[offset])) |
         (static_cast<std::uint32_t>(
              std::to_integer<std::uint8_t>(bytes[offset + 1u]))
          << 8u) |
         (static_cast<std::uint32_t>(
              std::to_integer<std::uint8_t>(bytes[offset + 2u]))
          << 16u) |
         (static_cast<std::uint32_t>(
              std::to_integer<std::uint8_t>(bytes[offset + 3u]))
          << 24u);
}

[[nodiscard]] std::uint64_t get_u64(std::span<const std::byte> bytes,
                                    std::size_t offset) noexcept {
  return static_cast<std::uint64_t>(
             std::to_integer<std::uint8_t>(bytes[offset])) |
         (static_cast<std::uint64_t>(
              std::to_integer<std::uint8_t>(bytes[offset + 1u]))
          << 8u) |
         (static_cast<std::uint64_t>(
              std::to_integer<std::uint8_t>(bytes[offset + 2u]))
          << 16u) |
         (static_cast<std::uint64_t>(
              std::to_integer<std::uint8_t>(bytes[offset + 3u]))
          << 24u) |
         (static_cast<std::uint64_t>(
              std::to_integer<std::uint8_t>(bytes[offset + 4u]))
          << 32u) |
         (static_cast<std::uint64_t>(
              std::to_integer<std::uint8_t>(bytes[offset + 5u]))
          << 40u) |
         (static_cast<std::uint64_t>(
              std::to_integer<std::uint8_t>(bytes[offset + 6u]))
          << 48u) |
         (static_cast<std::uint64_t>(
              std::to_integer<std::uint8_t>(bytes[offset + 7u]))
          << 56u);
}

[[nodiscard]] bool zero_range(std::span<const std::byte> bytes,
                              std::size_t begin,
                              std::size_t end) noexcept {
  return std::all_of(bytes.begin() + begin, bytes.begin() + end,
                     [](std::byte value) { return value == std::byte{}; });
}

// SnapshotModuleDescription embedded layout. All integers are little-endian.
// Offset  Size  Field
// ------  ----  -----
//      0     8  Module ID
//      8     4  Module schema version
//     12     4  SnapshotFileKind
//     16     8  Module snapshot file size
//     24     4  CRC-32 of the complete module snapshot file [0, file_size)
//     28     4  Reserved, zero
// Total serialized size: 32 bytes. There is no descriptor magic or standalone
// descriptor checksum; the enclosing SnapshotDescription CRC-32 covers it.
void put_module_description(std::span<std::byte> out, std::size_t offset,
                            const SnapshotModuleDescription& module) noexcept {
  put_u64(out, offset, module.module_id);
  put_u32(out, offset + 8u, module.schema_version);
  put_u32(out, offset + 12u,
          static_cast<std::uint32_t>(module.file_kind));
  put_u64(out, offset + 16u, module.file_size);
  put_u32(out, offset + 24u, module.checksum);
}

[[nodiscard]] SnapshotModuleDescription
get_module_description(std::span<const std::byte> bytes,
                       std::size_t offset) noexcept {
  return {get_u64(bytes, offset), get_u32(bytes, offset + 8u),
          static_cast<SnapshotFileKind>(get_u32(bytes, offset + 12u)),
          get_u64(bytes, offset + 16u), get_u32(bytes, offset + 24u)};
}

} // namespace

// HashChain snapshot layout. All integers are little-endian.
// Offset  Size  Field
// ------  ----  -----
//      0     4  Magic 0x36485346 ("FSH6")
//      4     2  Snapshot format version
//      6     2  HashChain schema version
//      8     8  HashChain module ID 0x3630305f48534148 ("HASH_006")
//     16     8  Capture generation ID
//     24     8  Snapshot-trigger record position
//     32     8  Processed end position (also restored into module state)
//     40     8  Snapshot-trigger record sequence
//     48     8  Hash-chain digest
//     56     1  Failed flag (0 or 1)
//     57     7  Reserved, zero
// Total serialized size: 64 bytes. No checksum is stored in this file; its
// CRC-32 covers bytes [0, 64) and is stored at SnapshotDescription offset 104.
HashChainSnapshotBytes
serialize_hash_chain_snapshot(const HashChainCapture& capture) noexcept {
  HashChainSnapshotBytes bytes{};
  put_u32(bytes, 0u, hash_snapshot_magic);
  put_u16(bytes, 4u, kSnapshotFormatVersion);
  put_u16(bytes, 6u, kHashChainSchemaVersion);
  put_u64(bytes, 8u, kHashChainModuleId);
  put_u64(bytes, 16u, capture.generation_id);
  put_u64(bytes, 24u, capture.record_position);
  put_u64(bytes, 32u, capture.processed_end);
  put_u64(bytes, 40u, capture.sequence);
  put_u64(bytes, 48u, capture.state.digest);
  bytes[56] = static_cast<std::byte>(capture.state.failed ? 1u : 0u);
  return bytes;
}

// BitAccumulator snapshot layout. All integers are little-endian.
// Offset  Size  Field
// ------  ----  -----
//      0     4  Magic 0x36425346 ("FSB6")
//      4     2  Snapshot format version
//      6     2  BitAccumulator schema version
//      8     8  BitAccumulator module ID 0x3630305f54494241 ("ABIT_006")
//     16     8  Capture generation ID
//     24     8  Snapshot-trigger record position
//     32     8  Processed end position (also restored into module state)
//     40     8  Snapshot-trigger record sequence
//     48     8  Total one-bit count
//     56     8  Rolling bit accumulator
//     64     1  Failed flag (0 or 1)
//     65     7  Reserved, zero
// Total serialized size: 72 bytes. No checksum is stored in this file; its
// CRC-32 covers bytes [0, 72) and is stored at SnapshotDescription offset 136.
BitAccumulatorSnapshotBytes serialize_bit_accumulator_snapshot(
    const BitAccumulatorCapture& capture) noexcept {
  BitAccumulatorSnapshotBytes bytes{};
  put_u32(bytes, 0u, bit_snapshot_magic);
  put_u16(bytes, 4u, kSnapshotFormatVersion);
  put_u16(bytes, 6u, kBitAccumulatorSchemaVersion);
  put_u64(bytes, 8u, kBitAccumulatorModuleId);
  put_u64(bytes, 16u, capture.generation_id);
  put_u64(bytes, 24u, capture.record_position);
  put_u64(bytes, 32u, capture.processed_end);
  put_u64(bytes, 40u, capture.sequence);
  put_u64(bytes, 48u, capture.state.total_one_bits);
  put_u64(bytes, 56u, capture.state.rolling_bits);
  bytes[64] = static_cast<std::byte>(capture.state.failed ? 1u : 0u);
  return bytes;
}

// SnapshotDescription layout. All integers are little-endian.
// Offset  Size  Field
// ------  ----  -----
//      0     4  Magic 0x36445346 ("FSD6")
//      4     2  Snapshot format version
//      6     2  Total serialized size (152)
//      8     8  Composition ID
//     16     8  Stream ID
//     24     8  Epoch ID
//     32     8  Manifest ID
//     40     8  Capture generation ID
//     48     8  Snapshot-trigger record position
//     56     8  Processed end position
//     64     8  Snapshot-trigger record sequence
//     72     4  Required module count (2)
//     76     2  WAL StreamKind
//     78     2  Reserved, zero
//     80    32  HashChain SnapshotModuleDescription
//    112    32  BitAccumulator SnapshotModuleDescription
//    144     4  CRC-32 covering bytes [0, 144)
//    148     4  Reserved, zero (not covered by the CRC-32)
// Total serialized size: 152 bytes. Each embedded module description contains
// its module snapshot checksum at relative offset 24 and reserved zeros at
// relative offsets [28, 32).
SnapshotDescriptionBytes serialize_snapshot_description(
    const SnapshotDescription& description) noexcept {
  SnapshotDescriptionBytes bytes{};
  put_u32(bytes, 0u, description_magic);
  put_u16(bytes, 4u, kSnapshotFormatVersion);
  put_u16(bytes, 6u,
          static_cast<std::uint16_t>(kSnapshotDescriptionSize));
  put_u64(bytes, 8u, description.identity.composition_id);
  put_u64(bytes, 16u, description.identity.stream_id);
  put_u64(bytes, 24u, description.identity.epoch_id);
  put_u64(bytes, 32u, description.identity.manifest_id);
  put_u64(bytes, 40u, description.generation_id);
  put_u64(bytes, 48u, description.record_position);
  put_u64(bytes, 56u, description.processed_end);
  put_u64(bytes, 64u, description.snapshot_sequence);
  put_u32(bytes, 72u, required_module_count);
  put_u16(bytes, 76u,
          static_cast<std::uint16_t>(description.identity.stream_kind));
  put_module_description(bytes, 80u, description.hash_chain);
  put_module_description(bytes, 112u, description.bit_accumulator);
  put_u32(bytes, 144u, wal::crc32_bytes(bytes.data(), 144u));
  return bytes;
}

bool deserialize_hash_chain_snapshot(std::span<const std::byte> bytes,
                                     HashChainCapture& capture) noexcept {
  if (bytes.size() != kHashChainSnapshotSize ||
      get_u32(bytes, 0u) != hash_snapshot_magic ||
      get_u16(bytes, 4u) != kSnapshotFormatVersion ||
      get_u16(bytes, 6u) != kHashChainSchemaVersion ||
      get_u64(bytes, 8u) != kHashChainModuleId ||
      std::to_integer<std::uint8_t>(bytes[56]) > 1u ||
      !zero_range(bytes, 57u, bytes.size())) {
    return false;
  }
  capture = {get_u64(bytes, 16u),
             get_u64(bytes, 24u),
             get_u64(bytes, 32u),
             get_u64(bytes, 40u),
             {get_u64(bytes, 32u), get_u64(bytes, 48u),
              bytes[56] != std::byte{}}};
  return true;
}

bool deserialize_bit_accumulator_snapshot(
    std::span<const std::byte> bytes,
    BitAccumulatorCapture& capture) noexcept {
  if (bytes.size() != kBitAccumulatorSnapshotSize ||
      get_u32(bytes, 0u) != bit_snapshot_magic ||
      get_u16(bytes, 4u) != kSnapshotFormatVersion ||
      get_u16(bytes, 6u) != kBitAccumulatorSchemaVersion ||
      get_u64(bytes, 8u) != kBitAccumulatorModuleId ||
      std::to_integer<std::uint8_t>(bytes[64]) > 1u ||
      !zero_range(bytes, 65u, bytes.size())) {
    return false;
  }
  capture = {get_u64(bytes, 16u),
             get_u64(bytes, 24u),
             get_u64(bytes, 32u),
             get_u64(bytes, 40u),
             {get_u64(bytes, 32u), get_u64(bytes, 48u),
              get_u64(bytes, 56u), bytes[64] != std::byte{}}};
  return true;
}

bool deserialize_snapshot_description(
    std::span<const std::byte> bytes,
    SnapshotDescription& description) noexcept {
  if (bytes.size() != kSnapshotDescriptionSize ||
      get_u32(bytes, 0u) != description_magic ||
      get_u16(bytes, 4u) != kSnapshotFormatVersion ||
      get_u16(bytes, 6u) != kSnapshotDescriptionSize ||
      get_u32(bytes, 72u) != required_module_count ||
      get_u16(bytes, 78u) != 0u || get_u32(bytes, 108u) != 0u ||
      get_u32(bytes, 140u) != 0u || get_u32(bytes, 148u) != 0u ||
      get_u32(bytes, 144u) != wal::crc32_bytes(bytes.data(), 144u)) {
    return false;
  }

  SnapshotDescription decoded{};
  decoded.identity.stream_kind =
      static_cast<wal::StreamKind>(get_u16(bytes, 76u));
  decoded.identity.composition_id = get_u64(bytes, 8u);
  decoded.identity.stream_id = get_u64(bytes, 16u);
  decoded.identity.epoch_id = get_u64(bytes, 24u);
  decoded.identity.manifest_id = get_u64(bytes, 32u);
  decoded.generation_id = get_u64(bytes, 40u);
  decoded.record_position = get_u64(bytes, 48u);
  decoded.processed_end = get_u64(bytes, 56u);
  decoded.snapshot_sequence = get_u64(bytes, 64u);
  decoded.hash_chain = get_module_description(bytes, 80u);
  decoded.bit_accumulator = get_module_description(bytes, 112u);
  if ((decoded.identity.stream_kind != wal::StreamKind::Generic &&
       decoded.identity.stream_kind != wal::StreamKind::Command &&
       decoded.identity.stream_kind != wal::StreamKind::Event) ||
      decoded.hash_chain.file_kind != SnapshotFileKind::HashChain ||
      decoded.bit_accumulator.file_kind !=
          SnapshotFileKind::BitAccumulator) {
    return false;
  }
  description = decoded;
  return true;
}

} // namespace fexma::snapshot_demo
