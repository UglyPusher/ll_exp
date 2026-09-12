#pragma once

/**
 * @file persistence.hpp
 * @brief Live physical persistence module without progress ownership.
 */

#include <fexma/wal/types.hpp>

#include <atomic>
#include <filesystem>
#include <memory>

namespace fexma::wal {

namespace detail {
class PhysicalWalAdapter;
}

struct PhysicalWalConfig {
  std::uint32_t payload_size{};
  std::uint32_t alignment{default_alignment};
  std::uint32_t payload_schema_version{};
  StreamKind stream_kind{StreamKind::Generic};
  StreamId stream_id{};
  EpochId epoch_id{};
  std::uint64_t first_sequence{1};
  ManifestId manifest_id{};
};

class PersistenceModule final {
public:
  PersistenceModule();
  ~PersistenceModule();

  PersistenceModule(const PersistenceModule&) = delete;
  PersistenceModule& operator=(const PersistenceModule&) = delete;
  PersistenceModule(PersistenceModule&&) = delete;
  PersistenceModule& operator=(PersistenceModule&&) = delete;

  [[nodiscard]] OpenResult open(const std::filesystem::path& path,
                                const PhysicalWalConfig& config) noexcept;
  [[nodiscard]] bool process(const RecordView& record) noexcept;
  [[nodiscard]] bool append(const RecordView& record) noexcept;
  [[nodiscard]] bool sync() noexcept;
  [[nodiscard]] bool close() noexcept;
  [[nodiscard]] bool is_open() const noexcept;
  [[nodiscard]] bool failed() const noexcept;

private:
  std::unique_ptr<detail::PhysicalWalAdapter> physical_wal_{};
  std::uint64_t first_sequence_{};
  std::atomic<bool> failed_{false};
};

} // namespace fexma::wal
