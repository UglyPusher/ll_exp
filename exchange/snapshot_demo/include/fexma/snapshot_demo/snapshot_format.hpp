#pragma once

/**
 * @file snapshot_format.hpp
 * @brief Canonical snapshot file formats for Demo 006.
 */

#include <fexma/snapshot_demo/capture_generation.hpp>
#include <fexma/wal/record_tape_types.hpp>
#include <fexma/wal/types.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace fexma::snapshot_demo {

inline constexpr std::uint16_t kSnapshotFormatVersion = 1u;
inline constexpr std::uint16_t kHashChainSchemaVersion = 1u;
inline constexpr std::uint16_t kBitAccumulatorSchemaVersion = 1u;

inline constexpr std::uint64_t kHashChainModuleId = 0x3630305f48534148ull;
inline constexpr std::uint64_t kBitAccumulatorModuleId =
    0x3630305f54494241ull;

inline constexpr std::size_t kHashChainSnapshotSize = 64u;
inline constexpr std::size_t kBitAccumulatorSnapshotSize = 72u;
inline constexpr std::size_t kSnapshotDescriptionSize = 152u;

using HashChainSnapshotBytes =
    std::array<std::byte, kHashChainSnapshotSize>;
using BitAccumulatorSnapshotBytes =
    std::array<std::byte, kBitAccumulatorSnapshotSize>;
using SnapshotDescriptionBytes =
    std::array<std::byte, kSnapshotDescriptionSize>;

enum class SnapshotFileKind : std::uint32_t {
  HashChain = 1u,
  BitAccumulator = 2u,
};

struct SnapshotIdentity {
  wal::StreamKind stream_kind{wal::StreamKind::Generic};
  wal::StreamId stream_id{};
  wal::EpochId epoch_id{};
  wal::ManifestId manifest_id{};
  std::uint64_t composition_id{};

  friend bool operator==(const SnapshotIdentity&,
                         const SnapshotIdentity&) = default;
};

struct SnapshotModuleDescription {
  std::uint64_t module_id{};
  std::uint32_t schema_version{};
  SnapshotFileKind file_kind{};
  std::uint64_t file_size{};
  std::uint32_t checksum{};

  friend bool operator==(const SnapshotModuleDescription&,
                         const SnapshotModuleDescription&) = default;
};

struct SnapshotDescription {
  SnapshotIdentity identity{};
  std::uint64_t generation_id{};
  wal::Position record_position{};
  wal::Position processed_end{};
  std::uint64_t snapshot_sequence{};
  SnapshotModuleDescription hash_chain{};
  SnapshotModuleDescription bit_accumulator{};

  friend bool operator==(const SnapshotDescription&,
                         const SnapshotDescription&) = default;
};

[[nodiscard]] HashChainSnapshotBytes
serialize_hash_chain_snapshot(const HashChainCapture& capture) noexcept;
[[nodiscard]] BitAccumulatorSnapshotBytes serialize_bit_accumulator_snapshot(
    const BitAccumulatorCapture& capture) noexcept;
[[nodiscard]] SnapshotDescriptionBytes
serialize_snapshot_description(const SnapshotDescription& description) noexcept;

[[nodiscard]] bool deserialize_hash_chain_snapshot(
    std::span<const std::byte> bytes, HashChainCapture& capture) noexcept;
[[nodiscard]] bool deserialize_bit_accumulator_snapshot(
    std::span<const std::byte> bytes, BitAccumulatorCapture& capture) noexcept;
[[nodiscard]] bool deserialize_snapshot_description(
    std::span<const std::byte> bytes, SnapshotDescription& description) noexcept;

} // namespace fexma::snapshot_demo
