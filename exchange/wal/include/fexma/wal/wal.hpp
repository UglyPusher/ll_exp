#pragma once

/**
 * @file wal.hpp
 * @brief Bounded in-memory queue with an explicit durability frontier.
 */

#include <fexma/wal/format.hpp>
#include <fexma/wal/types.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <span>

namespace fexma::wal {

class Wal final {
public:
  Wal() = default;
  ~Wal();

  Wal(const Wal&) = delete;
  Wal& operator=(const Wal&) = delete;
  Wal(Wal&&) = delete;
  Wal& operator=(Wal&&) = delete;

  [[nodiscard]] OpenResult open(const std::filesystem::path& path,
                                const WalConfig& config) noexcept;

  // Producer role. Copies one payload into the preallocated queue.
  [[nodiscard]] EnqueueResult
  try_enqueue(std::span<const std::byte> payload) noexcept;

  // Durability role. Persists accepted records in order, then flushes once.
  [[nodiscard]] DurabilityResult make_durable(
      std::uint32_t max_records = std::numeric_limits<std::uint32_t>::max())
      noexcept;

  // Consumer role. Copies and releases the oldest durable payload.
  [[nodiscard]] DequeueResult
  try_dequeue(std::span<std::byte> payload) noexcept;

  [[nodiscard]] CloseResult close() noexcept;

  [[nodiscard]] bool is_open() const noexcept;
  [[nodiscard]] const WalConfig& config() const noexcept;
  [[nodiscard]] std::uint64_t accepted_sequence() const noexcept;
  [[nodiscard]] std::uint64_t durable_sequence() const noexcept;

private:
  [[nodiscard]] std::byte* slot(std::uint64_t sequence) noexcept;
  [[nodiscard]] const std::byte* slot(std::uint64_t sequence) const noexcept;
  [[nodiscard]] bool write_record(std::uint64_t sequence,
                                  const std::byte* payload) noexcept;
  void release_storage() noexcept;

  std::ofstream stream_;
  WalConfig config_{};
  std::byte* storage_{};
  std::size_t slot_stride_{};
  std::atomic<std::uint64_t> accepted_sequence_{0};
  std::atomic<std::uint64_t> durable_sequence_{0};
  std::atomic<std::uint64_t> released_sequence_{0};
  std::atomic<bool> io_failed_{false};
  std::atomic<bool> open_{false};
};

} // namespace fexma::wal
