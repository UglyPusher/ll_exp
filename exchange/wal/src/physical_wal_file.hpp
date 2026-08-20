#pragma once

#include <fexma/wal/format.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <span>

namespace fexma::wal::detail {

class PhysicalWalAdapter final {
public:
  PhysicalWalAdapter() = default;
  ~PhysicalWalAdapter();

  PhysicalWalAdapter(const PhysicalWalAdapter&) = delete;
  PhysicalWalAdapter& operator=(const PhysicalWalAdapter&) = delete;

  [[nodiscard]] OpenStatus create(const std::filesystem::path& path,
                                  const WalConfig& config) noexcept;
  [[nodiscard]] bool append_record(
      std::uint64_t sequence,
      std::span<const std::byte> payload) noexcept;
  [[nodiscard]] bool sync() noexcept;
  [[nodiscard]] bool close() noexcept;
  [[nodiscard]] bool is_open() const noexcept;

private:
  [[nodiscard]] bool write_bytes(std::span<const std::byte> bytes) noexcept;

#if defined(_WIN32)
  void* handle_{};
#else
  int descriptor_{-1};
#endif
  WalConfig config_{};
};

enum class PhysicalReadStatus : std::uint8_t {
  Complete,
  EndOfFile,
  Incomplete,
  IoError
};

class PhysicalWalReaderAdapter final {
public:
  PhysicalWalReaderAdapter() = default;
  ~PhysicalWalReaderAdapter();

  PhysicalWalReaderAdapter(const PhysicalWalReaderAdapter&) = delete;
  PhysicalWalReaderAdapter& operator=(const PhysicalWalReaderAdapter&) = delete;

  [[nodiscard]] bool open(const std::filesystem::path& path) noexcept;
  [[nodiscard]] PhysicalReadStatus
  read(std::span<std::byte> bytes) noexcept;
  [[nodiscard]] bool close() noexcept;
  [[nodiscard]] bool is_open() const noexcept;

private:
#if defined(_WIN32)
  void* handle_{};
#else
  int descriptor_{-1};
#endif
};

inline constexpr std::uint64_t no_fault =
    std::numeric_limits<std::uint64_t>::max();

struct PhysicalWalFileTestControl {
  std::uint64_t append_calls{};
  std::uint64_t sync_calls{};
  std::uint64_t fail_append_call{no_fault};
  std::uint64_t fail_sync_call{no_fault};
};

void set_physical_wal_file_test_control(
    PhysicalWalFileTestControl* control) noexcept;

} // namespace fexma::wal::detail
