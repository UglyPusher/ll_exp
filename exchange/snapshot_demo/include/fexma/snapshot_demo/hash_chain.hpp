#pragma once

/**
 * @file hash_chain.hpp
 * @brief Deterministic ordered hash-chain module for Demo 006.
 */

#include <fexma/snapshot_demo/record.hpp>
#include <fexma/wal/types.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace fexma::snapshot_demo {

struct HashChainState {
  wal::Position processed_end{};
  std::uint64_t digest{14695981039346656037ull};
  bool failed{};

  friend bool operator==(const HashChainState&,
                         const HashChainState&) = default;
};

struct HashChainCapture {
  std::uint64_t generation_id{};
  wal::Position record_position{};
  wal::Position processed_end{};
  std::uint64_t sequence{};
  HashChainState state{};

  friend bool operator==(const HashChainCapture&,
                         const HashChainCapture&) = default;
};

class HashChainModule final {
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

    mix_byte(0xffu);
    mix_u64(record.position);
    mix_u64(record.sequence);
    mix_u64(static_cast<std::uint64_t>(record.payload.size()));
    for (const std::byte value : record.payload) {
      mix_byte(std::to_integer<std::uint8_t>(value));
    }
    ++state_.processed_end;
    if (decoded.record.kind == RecordKind::SaveSnapshot) {
      const auto capture_started = std::chrono::steady_clock::now();
      capture_.emplace(HashChainCapture{decoded.record.generation_id,
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

  [[nodiscard]] const HashChainState& state() const noexcept { return state_; }

  [[nodiscard]] const HashChainCapture* pending_capture() const noexcept {
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

  // Cold-path bootstrap: no slider or state reader may be active.
  void restore_quiescent(const HashChainState& state) noexcept {
    state_ = state;
    capture_.reset();
    capture_duration_ns_ = 0;
  }

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
  std::optional<HashChainCapture> capture_{};
  std::uint64_t capture_duration_ns_{};
};

} // namespace fexma::snapshot_demo
