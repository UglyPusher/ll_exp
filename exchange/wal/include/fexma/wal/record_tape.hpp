#pragma once

/**
 * @file record_tape.hpp
 * @brief Bounded in-memory RecordTape with head and tail boundaries.
 */

#include <fexma/wal/types.hpp>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>

namespace fexma::wal {

struct RecordTapeConfig {
  std::uint32_t payload_size{};
  std::uint32_t capacity{};
  std::uint32_t alignment{default_alignment};
};

enum class RecordTapeOpenStatus : std::uint8_t {
  Ok,
  InvalidConfig,
  AllocationFailed,
  AlreadyOpen
};

struct RecordTapeOpenResult {
  RecordTapeOpenStatus status{RecordTapeOpenStatus::InvalidConfig};

  [[nodiscard]] bool ok() const noexcept {
    return status == RecordTapeOpenStatus::Ok;
  }
};

enum class ReclaimStatus : std::uint8_t {
  Ok,
  Closed,
  InvalidPosition
};

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4324) // Intentional cache-line frontier isolation.
#endif

class RecordTape final {
public:
  RecordTape() = default;
  ~RecordTape();

  RecordTape(const RecordTape&) = delete;
  RecordTape& operator=(const RecordTape&) = delete;
  RecordTape(RecordTape&&) = delete;
  RecordTape& operator=(RecordTape&&) = delete;

  [[nodiscard]] RecordTapeOpenResult
  open(const RecordTapeConfig& config) noexcept;
  [[nodiscard]] PublishResult
  try_publish(std::span<const std::byte> payload) noexcept;
  [[nodiscard]] AccessResult try_view(Position position) const noexcept;

  // end is exclusive. The composition must ensure all mandatory readers have
  // finished every position below end before the sole reclaimer calls this.
  [[nodiscard]] ReclaimStatus reclaim(Position end) noexcept;

  // Cold-path lifecycle: producer, reclaimer, and view users must be stopped.
  void close() noexcept;

  [[nodiscard]] bool is_open() const noexcept;
  [[nodiscard]] Position head() const noexcept;
  [[nodiscard]] Position tail() const noexcept;

private:
  static constexpr std::size_t frontier_cache_line_size = 64;

  struct alignas(frontier_cache_line_size) Frontier {
    std::atomic<Position> value{0};
    std::array<std::byte,
               frontier_cache_line_size - sizeof(std::atomic<Position>)>
        padding{};
  };

  static_assert(std::atomic<Position>::is_always_lock_free);
  static_assert(sizeof(Frontier) == frontier_cache_line_size);

  class Storage final {
  public:
    Storage() = default;
    ~Storage();

    Storage(const Storage&) = delete;
    Storage& operator=(const Storage&) = delete;

    [[nodiscard]] RecordTapeOpenStatus
    initialize(const RecordTapeConfig& config) noexcept;
    void release() noexcept;

    [[nodiscard]] std::span<std::byte>
    block_at_slot(std::uint32_t slot) noexcept;
    [[nodiscard]] std::span<const std::byte>
    block_at_slot(std::uint32_t slot) const noexcept;
    [[nodiscard]] std::uint32_t next_slot(std::uint32_t slot) const noexcept;

  private:
    std::byte* data_{};
    std::size_t size_{};
    std::size_t stride_{};
    std::uint32_t payload_size_{};
    std::uint32_t capacity_{};
    std::uint32_t alignment_{default_alignment};
  };

  Frontier tail_frontier_{};
  Frontier head_frontier_{};
  std::uint32_t head_slot_{};
  Storage storage_{};
  RecordTapeConfig config_{};
  std::atomic<bool> open_{false};
};

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

} // namespace fexma::wal
