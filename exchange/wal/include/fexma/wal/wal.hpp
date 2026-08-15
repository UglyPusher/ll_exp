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
#include <optional>
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

  // Producer: copies one payload at write and advances write.
  [[nodiscard]] PushResult
  try_push(std::span<const std::byte> payload) noexcept;

  // Persists [durable, write), then advances durable.
  [[nodiscard]] DurabilityResult advance_durable(
      std::uint32_t max_records = std::numeric_limits<std::uint32_t>::max())
      noexcept;

  // Consumer: copies one payload at read and advances read.
  [[nodiscard]] PopResult try_pop(std::span<std::byte> payload) noexcept;

  [[nodiscard]] CloseResult close() noexcept;

  [[nodiscard]] bool is_open() const noexcept;
  [[nodiscard]] const WalConfig& config() const noexcept;
  [[nodiscard]] std::uint64_t read_cursor() const noexcept;
  [[nodiscard]] std::uint64_t durable_cursor() const noexcept;
  [[nodiscard]] std::uint64_t write_cursor() const noexcept;

private:
  struct WritableBlock {
    std::uint64_t position{};
    std::span<std::byte> bytes{};

    void fill(std::span<const std::byte> payload) const noexcept;
    [[nodiscard]] std::uint64_t sequence() const noexcept;
  };

  struct ReadableBlock {
    std::uint64_t position{};
    std::span<const std::byte> bytes{};

    void copy_to(std::span<std::byte> payload) const noexcept;
    [[nodiscard]] std::uint64_t sequence() const noexcept;
  };

  struct PositionRange {
    std::uint64_t begin{};
    std::uint64_t end{};

    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] std::uint32_t size() const noexcept;
  };

  [[nodiscard]] PushStatus
  check_push(std::span<const std::byte> payload) const noexcept;
  [[nodiscard]] std::optional<WritableBlock>
  try_acquire_writable_block() noexcept;
  void publish(const WritableBlock& block) noexcept;

  [[nodiscard]] DurabilityStatus check_durability() const noexcept;
  [[nodiscard]] PositionRange
  pending_range(std::uint32_t max_records) const noexcept;
  [[nodiscard]] bool persist(PositionRange range) noexcept;
  [[nodiscard]] DurabilityResult
  fail_durability(PositionRange range) noexcept;
  void publish_durable(PositionRange range) noexcept;

  [[nodiscard]] PopStatus
  check_pop(std::span<std::byte> payload) const noexcept;
  [[nodiscard]] std::optional<ReadableBlock>
  try_acquire_readable_block() noexcept;
  void release(const ReadableBlock& block) noexcept;

  [[nodiscard]] std::byte* slot(std::uint64_t position) noexcept;
  [[nodiscard]] const std::byte* slot(std::uint64_t position) const noexcept;
  [[nodiscard]] bool write_record(std::uint64_t sequence,
                                  const std::byte* payload) noexcept;
  void release_storage() noexcept;

  std::ofstream stream_;
  WalConfig config_{};
  std::byte* storage_{};
  std::size_t slot_stride_{};
  std::atomic<std::uint64_t> read_{0};
  std::atomic<std::uint64_t> durable_{0};
  std::atomic<std::uint64_t> write_{0};
  std::atomic<bool> io_failed_{false};
  std::atomic<bool> open_{false};
};

} // namespace fexma::wal
