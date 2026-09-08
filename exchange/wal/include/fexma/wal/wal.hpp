#pragma once

/**
 * @file wal.hpp
 * @brief Compatibility composition of the WAL core and persistence module.
 */

#include <fexma/wal/core.hpp>
#include <fexma/wal/format.hpp>
#include <fexma/wal/persistence.hpp>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <span>

namespace fexma::wal {

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4324) // Intentional cache-line frontier isolation.
#endif

// Transitional compatibility facade. WalCore owns the runtime string,
// PersistenceModule owns physical I/O, and this composition preserves the
// original three-role API until generic slider mechanics replace it.
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
  [[nodiscard]] AccessResult try_view(Position position) const noexcept;
  [[nodiscard]] CloseResult close() noexcept;

  [[nodiscard]] bool is_open() const noexcept;
  [[nodiscard]] const WalConfig& config() const noexcept;
  [[nodiscard]] WalSnapshot snapshot() const noexcept;

private:
  static constexpr std::size_t frontier_cache_line_size = 64;

  struct alignas(frontier_cache_line_size) Frontier {
    std::atomic<Position> value{0};
    std::array<std::byte,
               frontier_cache_line_size - sizeof(std::atomic<Position>)>
        padding{};
  };

  static_assert(sizeof(Frontier) == frontier_cache_line_size);

  void release_resources() noexcept;

  WalCore core_{};
  PersistenceModule persistence_{};
  Frontier durable_frontier_{};
  WalConfig config_{};
  std::atomic<bool> open_{false};

  friend class WalTestAccess;
};

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

} // namespace fexma::wal
