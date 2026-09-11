/**
 * @file test_little_endian.cpp
 * @brief Exact byte-layout checks for little-endian integer primitives.
 */

#include <fexma/binary/little_endian.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace {

[[nodiscard]] constexpr std::byte b(std::uint8_t value) noexcept {
  return static_cast<std::byte>(value);
}

template <fexma::binary::LittleEndianInteger UInt, std::size_t Size>
[[nodiscard]] bool check_value(
    UInt value, std::size_t offset,
    const std::array<std::byte, Size>& expected) noexcept {
  std::array<std::byte, 16> bytes{};
  bytes.fill(b(0xa5));

  fexma::binary::store_le(bytes, offset, value);
  for (std::size_t index = 0; index < bytes.size(); ++index) {
    const bool encoded = index >= offset && index < offset + expected.size();
    const std::byte wanted = encoded ? expected[index - offset] : b(0xa5);
    if (bytes[index] != wanted) {
      return false;
    }
  }

  return fexma::binary::load_le<UInt>(bytes, offset) == value;
}

} // namespace

static_assert(fexma::binary::LittleEndianInteger<std::uint16_t>);
static_assert(fexma::binary::LittleEndianInteger<std::uint32_t>);
static_assert(fexma::binary::LittleEndianInteger<std::uint64_t>);
static_assert(!fexma::binary::LittleEndianInteger<std::uint8_t>);
static_assert(!fexma::binary::LittleEndianInteger<std::int64_t>);

int main() {
  const bool u16 = check_value<std::uint16_t>(
      0x0102u, 3u, std::array{b(0x02), b(0x01)});
  const bool u32 = check_value<std::uint32_t>(
      0x01020304u, 5u,
      std::array{b(0x04), b(0x03), b(0x02), b(0x01)});
  const bool u64 = check_value<std::uint64_t>(
      0x0102030405060708ull, 1u,
      std::array{b(0x08), b(0x07), b(0x06), b(0x05), b(0x04), b(0x03),
                 b(0x02), b(0x01)});
  const bool zero = check_value<std::uint64_t>(
      0u, 8u, std::array<std::byte, 8>{});
  const bool maximum = check_value<std::uint32_t>(
      std::numeric_limits<std::uint32_t>::max(), 12u,
      std::array{b(0xff), b(0xff), b(0xff), b(0xff)});

  return u16 && u32 && u64 && zero && maximum ? 0 : 1;
}
