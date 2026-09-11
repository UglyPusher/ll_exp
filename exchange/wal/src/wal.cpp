/**
 * @file wal.cpp
 * @brief Compatibility composition of RecordTape and persistence module.
 */

#include <fexma/wal/wal.hpp>

#include <cstring>

namespace fexma::wal {

Wal::~Wal() {
  open_.store(false, std::memory_order_relaxed);
  release_resources();
}

OpenResult Wal::open(const std::filesystem::path& path,
                     const WalConfig& config) noexcept {
  if (is_open()) return {OpenStatus::AlreadyOpen};
  if (!valid_config(config)) return {OpenStatus::InvalidConfig};

  const OpenResult tape_opened = tape_.open(
      {config.payload_size, config.capacity, config.alignment,
       config.first_sequence});
  if (!tape_opened.ok()) return tape_opened;

  const OpenResult persistence_opened = persistence_.open(
      path, {config.payload_size, config.alignment,
             config.payload_schema_version, config.stream_kind,
             config.stream_id, config.epoch_id, config.first_sequence,
             config.manifest_id});
  if (!persistence_opened.ok()) {
    tape_.close();
    return persistence_opened;
  }

  config_ = config;
  durable_progress_.reset_quiescent(0);
  persistence_slider_.reset_quiescent(0);
  open_.store(true, std::memory_order_release);
  return {OpenStatus::Ok};
}

PublishResult Wal::try_publish(std::span<const std::byte> payload) noexcept {
  if (!is_open()) return {PublishStatus::Closed, 0};
  if (payload.size() != config_.payload_size) {
    return {PublishStatus::InvalidPayloadSize, 0};
  }
  if (tape_.sequence_exhausted()) {
    return {PublishStatus::SequenceExhausted, 0};
  }
  if (persistence_.failed()) return {PublishStatus::IoError, 0};
  return tape_.try_publish(payload);
}

DurabilityResult Wal::advance_durable(std::uint32_t batch_size) noexcept {
  const Position durable = durable_progress_.reader().acquire();
  if (!is_open()) return {DurabilityStatus::Closed, durable, 0};
  if (persistence_.failed()) return {DurabilityStatus::IoError, durable, 0};

  persistence_slider_.acquire_policy().set_maximum_count(batch_size);
  const SliderResult result = persistence_slider_.process_available();
  if (!result.ok()) return {DurabilityStatus::IoError, durable, 0};
  return {DurabilityStatus::Ok, durable_progress_.reader().acquire(),
          static_cast<std::uint32_t>(result.processed_count)};
}

ConsumeResult Wal::try_consume(std::span<std::byte> payload) noexcept {
  if (!is_open()) return {ConsumeStatus::Closed, 0};
  if (payload.size() != config_.payload_size) {
    return {ConsumeStatus::InvalidPayloadSize, 0};
  }

  const Position tail = tape_.tail();
  const Position durable = durable_progress_.reader().acquire();
  if (tail == durable) return {ConsumeStatus::Empty, 0};

  const AccessResult access = tape_.try_view(tail);
  if (!access.ok()) return {ConsumeStatus::Empty, 0};
  std::memcpy(payload.data(), access.record.payload.data(), payload.size());
  const std::uint64_t sequence = access.record.sequence;
  (void)tape_.reclaim(tail + 1u);
  return {ConsumeStatus::Ok, sequence};
}

AccessResult Wal::try_view(Position position) const noexcept {
  if (!is_open()) return {ViewStatus::Closed};
  return tape_.try_view(position);
}

CloseResult Wal::close() noexcept {
  if (!is_open()) return {CloseStatus::AlreadyClosed};

  const Position tail = tape_.tail();
  const Position durable = durable_progress_.reader().acquire();
  if (tail != durable) return {CloseStatus::PendingConsumption};

  const bool persistence_failed = persistence_.failed();
  if (!persistence_failed && durable != tape_.head()) {
    return {CloseStatus::PendingDurability};
  }

  open_.store(false, std::memory_order_release);
  const bool persistence_closed = persistence_.close();
  const bool sequence_exhausted = tape_.sequence_exhausted();
  tape_.close();
  if (!persistence_closed || persistence_failed) return {CloseStatus::IoError};
  return {sequence_exhausted ? CloseStatus::SequenceExhausted
                             : CloseStatus::Ok};
}

bool Wal::is_open() const noexcept {
  return open_.load(std::memory_order_acquire);
}

const WalConfig& Wal::config() const noexcept { return config_; }

WalSnapshot Wal::snapshot() const noexcept {
  const Position tail = tape_.tail();
  const Position durable = durable_progress_.reader().acquire();
  const Position head = tape_.head();
  return {tail, durable, head};
}

void Wal::release_resources() noexcept {
  (void)persistence_.close();
  tape_.close();
}

} // namespace fexma::wal
