/**
 * @file test_wal_core.cpp
 * @brief Contract tests for the persistence-free bounded WAL string.
 */

#include <fexma/wal/core.hpp>
#include <fexma/wal/persistence.hpp>
#include <fexma/wal/reader.hpp>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <thread>

using namespace fexma::wal;

namespace {

using Payload = std::array<std::byte, 16>;

[[nodiscard]] Payload payload(Position position) noexcept {
  Payload bytes{};
  for (std::size_t index = 0; index < bytes.size(); ++index) {
    bytes[index] =
        static_cast<std::byte>((position + index * 37u) & 0xffu);
  }
  return bytes;
}

[[nodiscard]] bool opens_without_physical_storage() {
  WalCore core;
  if (core.open({}).status != OpenStatus::InvalidConfig ||
      core.open({16, 0, 64, 1}).status != OpenStatus::InvalidConfig ||
      core.open({16, 4, 24, 1}).status != OpenStatus::InvalidConfig ||
      core.open({16, 4, 64, 0}).status != OpenStatus::InvalidConfig ||
      !core.open({16, 4, 64, 101}).ok() ||
      core.open({16, 4, 64, 101}).status != OpenStatus::AlreadyOpen) {
    return false;
  }

  const auto unexpected_path =
      std::filesystem::temp_directory_path() / "fexma_wal_core_has_no_file.wal";
  std::filesystem::remove(unexpected_path);
  const bool no_file = !std::filesystem::exists(unexpected_path);
  core.close();
  return no_file && !core.is_open() &&
         core.try_publish(payload(0)).status == PublishStatus::Closed &&
         core.try_view(0).status == ViewStatus::Closed &&
         core.reclaim(0) == ReclaimStatus::Closed;
}

[[nodiscard]] bool core_and_persistence_have_independent_lifecycles() {
  const auto path = std::filesystem::temp_directory_path() /
                    "fexma_wal_split_lifecycle.wal";
  std::filesystem::remove(path);
  constexpr WalConfig expected{16, 7, 64, 23, StreamKind::Command,
                               41, 43, 101, 47};
  WalCore core;
  PersistenceModule persistence;
  if (!core.open({expected.payload_size, expected.capacity,
                  expected.alignment, expected.first_sequence})
           .ok() ||
      !persistence
           .open(path, {expected.payload_size, expected.alignment,
                        expected.payload_schema_version, expected.stream_kind,
                        expected.stream_id, expected.epoch_id,
                        expected.first_sequence, expected.manifest_id})
           .ok()) {
    return false;
  }

  for (Position position = 0; position < 3; ++position) {
    if (!core.try_publish(payload(position)).ok()) return false;
    const AccessResult access = core.try_view(position);
    if (!access.ok() || !persistence.append(access.record)) return false;
  }
  if (!persistence.sync() || !persistence.close() || persistence.failed()) {
    return false;
  }

  // Runtime storage remains open and readable after persistence closes.
  if (!core.is_open() || !core.try_view(2).ok()) return false;
  core.close();

  WalReader reader;
  if (!reader.open(path, expected).ok()) return false;
  Payload bytes{};
  for (Position position = 0; position < 3; ++position) {
    const ReadResult read = reader.read_next(bytes);
    if (!read.ok() || read.sequence != expected.first_sequence + position ||
        bytes != payload(position)) {
      return false;
    }
  }
  const bool valid = reader.read_next(bytes).status == ReadStatus::EndOfLog &&
                     reader.close();
  std::filesystem::remove(path);
  return valid;
}

[[nodiscard]] bool reclaims_only_valid_absolute_ranges() {
  WalCore core;
  if (!core.open({16, 3, 64, 11}).ok()) return false;

  for (Position position = 0; position < 3; ++position) {
    const PublishResult published = core.try_publish(payload(position));
    if (!published.ok() || published.sequence != position + 11) return false;
  }
  if (core.try_publish(payload(3)).status != PublishStatus::Full ||
      core.reclaim(4) != ReclaimStatus::InvalidPosition ||
      core.reclaim(2) != ReclaimStatus::Ok || core.tail() != 2 ||
      core.reclaim(1) != ReclaimStatus::InvalidPosition ||
      core.try_view(1).status != ViewStatus::Reclaimed ||
      !core.try_publish(payload(3)).ok() ||
      !core.try_publish(payload(4)).ok()) {
    return false;
  }

  for (Position position = 2; position < 5; ++position) {
    const AccessResult access = core.try_view(position);
    if (!access.ok() || access.record.position != position ||
        access.record.sequence != position + 11 ||
        access.record.payload.size() != 16) {
      return false;
    }
    for (std::size_t index = 0; index < access.record.payload.size(); ++index) {
      if (access.record.payload[index] != payload(position)[index]) return false;
    }
  }
  if (core.reclaim(5) != ReclaimStatus::Ok || core.tail() != core.head()) {
    return false;
  }
  core.close();
  return true;
}

[[nodiscard]] bool sequence_exhaustion_is_core_state() {
  constexpr std::uint64_t maximum =
      std::numeric_limits<std::uint64_t>::max();
  WalCore core;
  if (!core.open({16, 2, 64, maximum}).ok()) return false;
  const PublishResult last = core.try_publish(payload(0));
  const PublishResult exhausted = core.try_publish(payload(1));
  const AccessResult retained = core.try_view(0);
  const bool valid = last.ok() && last.sequence == maximum &&
                     exhausted.status == PublishStatus::SequenceExhausted &&
                     core.sequence_exhausted() && retained.ok() &&
                     retained.record.sequence == maximum;
  core.close();
  return valid;
}

[[nodiscard]] bool producer_and_reclaimer_wrap_concurrently() {
  constexpr Position count = 50'000;
  WalCore core;
  if (!core.open({16, 128, 64, 1}).ok()) return false;
  std::atomic<bool> failed{false};

  std::thread producer([&] {
    for (Position position = 0; position < count;) {
      const PublishResult published = core.try_publish(payload(position));
      if (published.ok()) {
        if (published.sequence != position + 1) failed = true;
        ++position;
      } else if (published.status != PublishStatus::Full) {
        failed = true;
        return;
      } else {
        std::this_thread::yield();
      }
    }
  });

  std::thread reclaimer([&] {
    for (Position position = 0; position < count;) {
      const AccessResult access = core.try_view(position);
      if (access.status == ViewStatus::Unpublished) {
        std::this_thread::yield();
        continue;
      }
      if (!access.ok() || access.record.sequence != position + 1) {
        failed = true;
        return;
      }
      const Payload expected = payload(position);
      for (std::size_t index = 0; index < expected.size(); ++index) {
        if (access.record.payload[index] != expected[index]) {
          failed = true;
          return;
        }
      }
      // The borrowed view is no longer used after this statement.
      if (core.reclaim(position + 1) != ReclaimStatus::Ok) {
        failed = true;
        return;
      }
      ++position;
    }
  });

  producer.join();
  reclaimer.join();
  const bool valid = !failed && core.tail() == count && core.head() == count;
  core.close();
  return valid;
}

} // namespace

int main() {
  if (!opens_without_physical_storage()) return 1;
  if (!core_and_persistence_have_independent_lifecycles()) return 2;
  if (!reclaims_only_valid_absolute_ranges()) return 3;
  if (!sequence_exhaustion_is_core_state()) return 4;
  if (!producer_and_reclaimer_wrap_concurrently()) return 5;
  return 0;
}
