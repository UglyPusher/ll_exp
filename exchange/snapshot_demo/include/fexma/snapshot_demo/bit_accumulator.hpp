#pragma once

/**
 * @file bit_accumulator.hpp
 * @brief Deterministic ordered bit-accumulator module for Demo 006.
 */

#include <fexma/snapshot_demo/record.hpp>
#include <fexma/wal/types.hpp>

#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace fexma::snapshot_demo {

struct BitAccumulatorState {
  wal::Position processed_end{};
  std::uint64_t total_one_bits{};
  std::uint64_t rolling_bits{0x9e3779b97f4a7c15ull};
  bool failed{};

  friend bool operator==(const BitAccumulatorState&,
                         const BitAccumulatorState&) = default;
};

struct BitAccumulatorCapture {
  std::uint64_t generation_id{};
  wal::Position record_position{};
  wal::Position processed_end{};
  std::uint64_t sequence{};
  BitAccumulatorState state{};

  friend bool operator==(const BitAccumulatorCapture&,
                         const BitAccumulatorCapture&) = default;
};

class BitAccumulatorModule final {
public:
  [[nodiscard]] bool process(const wal::RecordView& record) noexcept {
    if (state_.failed || record.position != state_.processed_end) {
      state_.failed = true;
      return false;
    }

    const DecodeResult decoded = decode_record(record.payload);
    if (!decoded) {
      state_.failed = true;
      return false;
    }
    if (decoded.record.kind == RecordKind::SaveSnapshot &&
        decoded.record.generation_id != record.position) {
      state_.failed = true;
      return false;
    }
    if (decoded.record.kind == RecordKind::SaveSnapshot && capture_) {
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
    if (decoded.record.kind == RecordKind::SaveSnapshot) {
      const auto capture_started = std::chrono::steady_clock::now();
      capture_.emplace(BitAccumulatorCapture{decoded.record.generation_id,
                                             record.position,
                                             state_.processed_end,
                                             record.sequence, state_});
      capture_duration_ns_ = static_cast<std::uint64_t>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(
              std::chrono::steady_clock::now() - capture_started)
              .count());
    }
    return true;
  }

  [[nodiscard]] const BitAccumulatorState& state() const noexcept {
    return state_;
  }

  [[nodiscard]] const BitAccumulatorCapture* pending_capture() const noexcept {
    return capture_ ? &*capture_ : nullptr;
  }

  [[nodiscard]] std::uint64_t capture_duration_ns() const noexcept {
    return capture_duration_ns_;
  }

  [[nodiscard]] bool release_capture(std::uint64_t generation_id,
                                     wal::Position processed_end) noexcept {
    if (!capture_ || capture_->generation_id != generation_id ||
        capture_->processed_end != processed_end) {
      return false;
    }
    capture_.reset();
    return true;
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
  std::optional<BitAccumulatorCapture> capture_{};
  std::uint64_t capture_duration_ns_{};
};

} // namespace fexma::snapshot_demo
