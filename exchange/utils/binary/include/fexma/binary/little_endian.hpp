#pragma once

/**
 * @file little_endian.hpp
 * @brief Unaligned little-endian integer loads and stores.
 */

#include <cassert>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <span>

namespace fexma::binary {

template <typename UInt>
concept LittleEndianInteger =
    std::same_as<UInt, std::uint16_t> ||
    std::same_as<UInt, std::uint32_t> ||
    std::same_as<UInt, std::uint64_t>;

/**
 * Stores an unsigned integer in canonical little-endian byte order.
 *
 * Precondition: [offset, offset + sizeof(UInt)) is within bytes.
 */
template <LittleEndianInteger UInt>
inline void store_le(std::span<std::byte> bytes, std::size_t offset,
                     UInt value) noexcept {
  assert(offset <= bytes.size());
  assert(sizeof(UInt) <= bytes.size() - offset);

  for (std::size_t index = 0; index < sizeof(UInt); ++index) {
    bytes[offset + index] =
        static_cast<std::byte>((value >> (index * 8u)) & UInt{0xffu});
  }
}

/**
 * Loads an unsigned integer from canonical little-endian byte order.
 *
 * Precondition: [offset, offset + sizeof(UInt)) is within bytes.
 */
template <LittleEndianInteger UInt>
[[nodiscard]] inline UInt load_le(std::span<const std::byte> bytes,
                                  std::size_t offset) noexcept {
  assert(offset <= bytes.size());
  assert(sizeof(UInt) <= bytes.size() - offset);

  UInt value{};
  for (std::size_t index = 0; index < sizeof(UInt); ++index) {
    value |= static_cast<UInt>(
                 std::to_integer<std::uint8_t>(bytes[offset + index]))
             << (index * 8u);
  }
  return value;
}

} // namespace fexma::binary
