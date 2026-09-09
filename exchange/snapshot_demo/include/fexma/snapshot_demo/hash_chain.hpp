#pragma once

/**
 * @file hash_chain.hpp
 * @brief Deterministic ordered hash-chain module for Demo 006.
 */

#include <fexma/wal/types.hpp>

#include <cstddef>
#include <cstdint>

namespace fexma::snapshot_demo {

struct HashChainState {
  wal::Position processed_end{};
  std::uint64_t digest{14695981039346656037ull};
  bool failed{};

  friend bool operator==(const HashChainState&,
                         const HashChainState&) = default;
};

class HashChainModule final {
public:
  [[nodiscard]] bool process(const wal::RecordView& record) noexcept {
    if (state_.failed || record.position != state_.processed_end) {
      state_.failed = true;
      return false;
    }

    mix_byte(0xffu);
    mix_u64(record.position);
    mix_u64(record.sequence);
    mix_u64(static_cast<std::uint64_t>(record.payload.size()));
    for (const std::byte value : record.payload) {
      mix_byte(std::to_integer<std::uint8_t>(value));
    }
    ++state_.processed_end;
    return true;
  }

  [[nodiscard]] const HashChainState& state() const noexcept { return state_; }

private:
  void mix_byte(std::uint8_t value) noexcept {
    state_.digest ^= value;
    state_.digest *= 1099511628211ull;
  }

  void mix_u64(std::uint64_t value) noexcept {
    for (std::size_t byte = 0; byte < sizeof(value); ++byte) {
      mix_byte(static_cast<std::uint8_t>((value >> (byte * 8u)) & 0xffu));
    }
  }

  HashChainState state_{};
};

} // namespace fexma::snapshot_demo
