/**
 * @file persistence.cpp
 * @brief Live physical persistence module implementation.
 */

#include <fexma/wal/format.hpp>
#include <fexma/wal/persistence.hpp>

#include "physical_wal_adapter.hpp"

#include <limits>
#include <new>

namespace fexma::wal {

PersistenceModule::PersistenceModule() = default;

PersistenceModule::~PersistenceModule() { (void)close(); }

OpenResult PersistenceModule::open(const std::filesystem::path& path,
                                   const PhysicalWalConfig& config) noexcept {
  if (is_open()) return {OpenStatus::AlreadyOpen};
  const WalConfig adapter_config{
      config.payload_size, 1, config.alignment, config.payload_schema_version,
      config.stream_kind, config.stream_id, config.epoch_id,
      config.first_sequence, config.manifest_id};
  if (!valid_config(adapter_config)) return {OpenStatus::InvalidConfig};

  physical_wal_.reset(new (std::nothrow) detail::PhysicalWalAdapter{});
  if (!physical_wal_) return {OpenStatus::AllocationFailed};
  const OpenStatus status = physical_wal_->create(path, adapter_config);
  if (status != OpenStatus::Ok) {
    physical_wal_.reset();
    return {status};
  }
  first_sequence_ = config.first_sequence;
  failed_.store(false, std::memory_order_relaxed);
  return {OpenStatus::Ok};
}

bool PersistenceModule::process(const RecordView& record) noexcept {
  return append(record);
}

bool PersistenceModule::append(const RecordView& record) noexcept {
  if (!is_open() || failed_.load(std::memory_order_acquire) ||
      record.position > std::numeric_limits<std::uint64_t>::max() -
                            first_sequence_ ||
      !physical_wal_->append_record(first_sequence_ + record.position,
                                    record.payload)) {
    failed_.store(true, std::memory_order_release);
    return false;
  }
  return true;
}

bool PersistenceModule::sync() noexcept {
  if (!is_open() || failed_.load(std::memory_order_acquire) ||
      !physical_wal_->sync()) {
    failed_.store(true, std::memory_order_release);
    return false;
  }
  return true;
}

bool PersistenceModule::close() noexcept {
  if (!physical_wal_) return true;
  const bool closed = physical_wal_->close();
  physical_wal_.reset();
  return closed;
}

bool PersistenceModule::is_open() const noexcept {
  return physical_wal_ && physical_wal_->is_open();
}

bool PersistenceModule::failed() const noexcept {
  return failed_.load(std::memory_order_acquire);
}

} // namespace fexma::wal
