#pragma once

/**
 * @file wal.hpp
 * @brief Bounded WAL frontier ring with an explicit durability stage.
 */

#include <fexma/wal/format.hpp>
#include <fexma/wal/types.hpp>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <span>

namespace fexma::wal {

namespace detail {
class PhysicalWalAdapter;
}

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4324) // Intentional cache-line frontier isolation.
#endif

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

  [[nodiscard]] PublishResult
  try_publish(std::span<const std::byte> payload) noexcept;

  [[nodiscard]] DurabilityResult advance_durable(
      std::uint32_t batch_size = std::numeric_limits<std::uint32_t>::max())
      noexcept;

  [[nodiscard]] ConsumeResult try_consume(std::span<std::byte> payload) noexcept;

  [[nodiscard]] CloseResult close() noexcept;

  [[nodiscard]] bool is_open() const noexcept;
  [[nodiscard]] const WalConfig& config() const noexcept;
  [[nodiscard]] WalSnapshot snapshot() const noexcept;

private:
  static constexpr std::size_t frontier_cache_line_size = 64;

  struct alignas(frontier_cache_line_size) Frontier {
    std::atomic<std::uint64_t> value{0};
    std::array<std::byte,
               frontier_cache_line_size - sizeof(std::atomic<std::uint64_t>)>
        padding{};
  };

  static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
  static_assert(sizeof(Frontier) == frontier_cache_line_size);

  class Storage final {
  public:
    Storage() = default;
    ~Storage();

    Storage(const Storage&) = delete;
    Storage& operator=(const Storage&) = delete;

    [[nodiscard]] OpenStatus initialize(const WalConfig& config) noexcept;
    void release() noexcept;

    [[nodiscard]] std::span<std::byte> block_at_slot(std::uint32_t slot) noexcept;
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

  void release_resources() noexcept;

  // Frontiers are absolute positions used for ordering, capacity checks,
  // sequences, and cross-thread publication.
  Frontier tail_frontier_{};
  Frontier durable_frontier_{};
  Frontier head_frontier_{};

  // Slots are cached ring indexes used only to address storage without
  // recomputing position % capacity on the hot path.
  std::uint32_t tail_slot_{};
  std::uint32_t durable_slot_{};
  std::uint32_t head_slot_{};
  Storage storage_{};
  std::unique_ptr<detail::PhysicalWalAdapter> physical_wal_{};
  WalConfig config_{};
  std::atomic<bool> io_failed_{false};
  std::atomic<bool> open_{false};

  friend class WalTestAccess;
};

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

} // namespace fexma::wal
