#pragma once

/**
 * @file bit_accumulator.hpp
 * @brief Deterministic ordered bit-accumulator module for Demo 006.
 */

#include <fexma/wal/types.hpp>

#include <bit>
#include <cstddef>
#include <cstdint>

namespace fexma::snapshot_demo {

struct BitAccumulatorState {
  wal::Position processed_end{};
  std::uint64_t total_one_bits{};
  std::uint64_t rolling_bits{0x9e3779b97f4a7c15ull};
  bool failed{};

  friend bool operator==(const BitAccumulatorState&,
                         const BitAccumulatorState&) = default;
};

class BitAccumulatorModule final {
public:
  [[nodiscard]] bool process(const wal::RecordView& record) noexcept {
    if (state_.failed || record.position != state_.processed_end) {
      state_.failed = true;
      return false;
    }

    fold_u64(record.position);
    fold_u64(record.sequence);
    fold_u64(static_cast<std::uint64_t>(record.payload.size()));
    for (const std::byte value : record.payload) {
      const std::uint8_t byte = std::to_integer<std::uint8_t>(value);
      state_.total_one_bits += std::popcount(byte);
      fold_byte(byte);
    }
    ++state_.processed_end;
    return true;
  }

  [[nodiscard]] const BitAccumulatorState& state() const noexcept {
    return state_;
  }

private:
  void fold_byte(std::uint8_t value) noexcept {
    state_.rolling_bits =
        std::rotl(state_.rolling_bits, 7) ^
        (static_cast<std::uint64_t>(value) + 0xa0761d6478bd642full);
  }

  void fold_u64(std::uint64_t value) noexcept {
    for (std::size_t byte = 0; byte < sizeof(value); ++byte) {
      fold_byte(
          static_cast<std::uint8_t>((value >> (byte * 8u)) & 0xffu));
    }
  }

  BitAccumulatorState state_{};
};

} // namespace fexma::snapshot_demo
