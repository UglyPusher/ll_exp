/**
 * @file test_snapshot_format.cpp
 * @brief Exact byte-layout checks for Demo 006 snapshot formats.
 */

#include <fexma/snapshot_demo/snapshot_format.hpp>

#include <fexma/wal/format.hpp>

#include <cstddef>
#include <cstdint>

using namespace fexma;

namespace {

[[nodiscard]] constexpr std::byte b(std::uint8_t value) noexcept {
  return static_cast<std::byte>(value);
}

void rewrite_description_crc(
    snapshot_demo::SnapshotDescriptionBytes& bytes) noexcept {
  const std::uint32_t crc = wal::crc32_bytes(bytes.data(), 144u);
  bytes[144] = b(static_cast<std::uint8_t>(crc & 0xffu));
  bytes[145] = b(static_cast<std::uint8_t>((crc >> 8u) & 0xffu));
  bytes[146] = b(static_cast<std::uint8_t>((crc >> 16u) & 0xffu));
  bytes[147] = b(static_cast<std::uint8_t>((crc >> 24u) & 0xffu));
}

[[nodiscard]] bool hash_chain_layout_is_canonical() noexcept {
  constexpr snapshot_demo::HashChainCapture capture{
      0x0706050403020100ull,
      0x1716151413121110ull,
      0x2726252423222120ull,
      0x3736353433323130ull,
      {0x2726252423222120ull, 0x4746454443424140ull, true}};
  constexpr snapshot_demo::HashChainSnapshotBytes expected{
      b(0x46), b(0x53), b(0x48), b(0x36), b(0x01), b(0x00), b(0x01), b(0x00),
      b(0x48), b(0x41), b(0x53), b(0x48), b(0x5f), b(0x30), b(0x30), b(0x36),
      b(0x00), b(0x01), b(0x02), b(0x03), b(0x04), b(0x05), b(0x06), b(0x07),
      b(0x10), b(0x11), b(0x12), b(0x13), b(0x14), b(0x15), b(0x16), b(0x17),
      b(0x20), b(0x21), b(0x22), b(0x23), b(0x24), b(0x25), b(0x26), b(0x27),
      b(0x30), b(0x31), b(0x32), b(0x33), b(0x34), b(0x35), b(0x36), b(0x37),
      b(0x40), b(0x41), b(0x42), b(0x43), b(0x44), b(0x45), b(0x46), b(0x47),
      b(0x01), b(0x00), b(0x00), b(0x00), b(0x00), b(0x00), b(0x00), b(0x00)};

  snapshot_demo::HashChainCapture decoded{};
  if (snapshot_demo::serialize_hash_chain_snapshot(capture) != expected ||
      !snapshot_demo::deserialize_hash_chain_snapshot(expected, decoded) ||
      decoded != capture) {
    return false;
  }

  auto nonzero_reserved = expected;
  nonzero_reserved[57] = b(0x01);
  return !snapshot_demo::deserialize_hash_chain_snapshot(nonzero_reserved,
                                                         decoded);
}

[[nodiscard]] bool bit_accumulator_layout_is_canonical() noexcept {
  constexpr snapshot_demo::BitAccumulatorCapture capture{
      0x0706050403020100ull,
      0x1716151413121110ull,
      0x2726252423222120ull,
      0x3736353433323130ull,
      {0x2726252423222120ull, 0x4746454443424140ull,
       0x5756555453525150ull, true}};
  constexpr snapshot_demo::BitAccumulatorSnapshotBytes expected{
      b(0x46), b(0x53), b(0x42), b(0x36), b(0x01), b(0x00), b(0x01), b(0x00),
      b(0x41), b(0x42), b(0x49), b(0x54), b(0x5f), b(0x30), b(0x30), b(0x36),
      b(0x00), b(0x01), b(0x02), b(0x03), b(0x04), b(0x05), b(0x06), b(0x07),
      b(0x10), b(0x11), b(0x12), b(0x13), b(0x14), b(0x15), b(0x16), b(0x17),
      b(0x20), b(0x21), b(0x22), b(0x23), b(0x24), b(0x25), b(0x26), b(0x27),
      b(0x30), b(0x31), b(0x32), b(0x33), b(0x34), b(0x35), b(0x36), b(0x37),
      b(0x40), b(0x41), b(0x42), b(0x43), b(0x44), b(0x45), b(0x46), b(0x47),
      b(0x50), b(0x51), b(0x52), b(0x53), b(0x54), b(0x55), b(0x56), b(0x57),
      b(0x01), b(0x00), b(0x00), b(0x00), b(0x00), b(0x00), b(0x00), b(0x00)};

  snapshot_demo::BitAccumulatorCapture decoded{};
  if (snapshot_demo::serialize_bit_accumulator_snapshot(capture) != expected ||
      !snapshot_demo::deserialize_bit_accumulator_snapshot(expected, decoded) ||
      decoded != capture) {
    return false;
  }

  auto nonzero_reserved = expected;
  nonzero_reserved[65] = b(0x01);
  return !snapshot_demo::deserialize_bit_accumulator_snapshot(nonzero_reserved,
                                                              decoded);
}

[[nodiscard]] bool description_layout_is_canonical() noexcept {
  constexpr snapshot_demo::SnapshotDescription description{
      {wal::StreamKind::Event, 0x1716151413121110ull,
       0x2726252423222120ull, 0x3736353433323130ull,
       0x0706050403020100ull},
      0x4746454443424140ull,
      0x5756555453525150ull,
      0x6766656463626160ull,
      0x7776757473727170ull,
      {snapshot_demo::kHashChainModuleId,
       snapshot_demo::kHashChainSchemaVersion,
       snapshot_demo::SnapshotFileKind::HashChain, 64u, 0x83828180u},
      {snapshot_demo::kBitAccumulatorModuleId,
       snapshot_demo::kBitAccumulatorSchemaVersion,
       snapshot_demo::SnapshotFileKind::BitAccumulator, 72u, 0x93929190u}};
  constexpr snapshot_demo::SnapshotDescriptionBytes expected{
      b(0x46), b(0x53), b(0x44), b(0x36), b(0x01), b(0x00), b(0x98), b(0x00),
      b(0x00), b(0x01), b(0x02), b(0x03), b(0x04), b(0x05), b(0x06), b(0x07),
      b(0x10), b(0x11), b(0x12), b(0x13), b(0x14), b(0x15), b(0x16), b(0x17),
      b(0x20), b(0x21), b(0x22), b(0x23), b(0x24), b(0x25), b(0x26), b(0x27),
      b(0x30), b(0x31), b(0x32), b(0x33), b(0x34), b(0x35), b(0x36), b(0x37),
      b(0x40), b(0x41), b(0x42), b(0x43), b(0x44), b(0x45), b(0x46), b(0x47),
      b(0x50), b(0x51), b(0x52), b(0x53), b(0x54), b(0x55), b(0x56), b(0x57),
      b(0x60), b(0x61), b(0x62), b(0x63), b(0x64), b(0x65), b(0x66), b(0x67),
      b(0x70), b(0x71), b(0x72), b(0x73), b(0x74), b(0x75), b(0x76), b(0x77),
      b(0x02), b(0x00), b(0x00), b(0x00), b(0x02), b(0x00), b(0x00), b(0x00),
      b(0x48), b(0x41), b(0x53), b(0x48), b(0x5f), b(0x30), b(0x30), b(0x36),
      b(0x01), b(0x00), b(0x00), b(0x00), b(0x01), b(0x00), b(0x00), b(0x00),
      b(0x40), b(0x00), b(0x00), b(0x00), b(0x00), b(0x00), b(0x00), b(0x00),
      b(0x80), b(0x81), b(0x82), b(0x83), b(0x00), b(0x00), b(0x00), b(0x00),
      b(0x41), b(0x42), b(0x49), b(0x54), b(0x5f), b(0x30), b(0x30), b(0x36),
      b(0x01), b(0x00), b(0x00), b(0x00), b(0x02), b(0x00), b(0x00), b(0x00),
      b(0x48), b(0x00), b(0x00), b(0x00), b(0x00), b(0x00), b(0x00), b(0x00),
      b(0x90), b(0x91), b(0x92), b(0x93), b(0x00), b(0x00), b(0x00), b(0x00),
      b(0xe4), b(0x0f), b(0x25), b(0xf8), b(0x00), b(0x00), b(0x00), b(0x00)};

  snapshot_demo::SnapshotDescription decoded{};
  if (snapshot_demo::serialize_snapshot_description(description) != expected ||
      !snapshot_demo::deserialize_snapshot_description(expected, decoded) ||
      decoded != description) {
    return false;
  }

  auto covered_byte_changed = expected;
  covered_byte_changed[8] ^= b(0x01);
  if (snapshot_demo::deserialize_snapshot_description(covered_byte_changed,
                                                      decoded)) {
    return false;
  }
  rewrite_description_crc(covered_byte_changed);
  auto changed_description = description;
  changed_description.identity.composition_id ^= 1u;
  if (!snapshot_demo::deserialize_snapshot_description(covered_byte_changed,
                                                       decoded) ||
      decoded != changed_description) {
    return false;
  }

  auto header_reserved = expected;
  header_reserved[78] = b(0x01);
  rewrite_description_crc(header_reserved);
  if (snapshot_demo::deserialize_snapshot_description(header_reserved,
                                                      decoded)) {
    return false;
  }

  auto hash_reserved = expected;
  hash_reserved[108] = b(0x01);
  rewrite_description_crc(hash_reserved);
  if (snapshot_demo::deserialize_snapshot_description(hash_reserved,
                                                      decoded)) {
    return false;
  }

  auto bit_reserved = expected;
  bit_reserved[140] = b(0x01);
  rewrite_description_crc(bit_reserved);
  if (snapshot_demo::deserialize_snapshot_description(bit_reserved, decoded)) {
    return false;
  }

  auto trailer_reserved = expected;
  trailer_reserved[148] = b(0x01);
  return !snapshot_demo::deserialize_snapshot_description(trailer_reserved,
                                                          decoded);
}

} // namespace

int main() {
  if (!hash_chain_layout_is_canonical()) return 1;
  if (!bit_accumulator_layout_is_canonical()) return 2;
  if (!description_layout_is_canonical()) return 3;
  return 0;
}
