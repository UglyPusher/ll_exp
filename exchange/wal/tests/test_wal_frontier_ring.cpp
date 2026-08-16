/**
 * @file test_wal_frontier_ring.cpp
 * @brief Contract tests for the fixed-payload WAL frontier ring.
 */

#include <fexma/wal/wal.hpp>

#include "physical_wal_file.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <new>
#include <span>
#include <thread>
#include <type_traits>

#if defined(_MSC_VER)
#include <malloc.h>
#endif

namespace {

std::atomic<bool> track_allocations{false};
std::atomic<std::uint64_t> allocation_count{0};

void count_allocation() noexcept {
  if (track_allocations.load(std::memory_order_relaxed)) {
    allocation_count.fetch_add(1, std::memory_order_relaxed);
  }
}

[[nodiscard]] void* allocate(std::size_t size) {
  count_allocation();
  if (void* memory = std::malloc(size == 0 ? 1 : size)) {
    return memory;
  }
  throw std::bad_alloc{};
}

[[nodiscard]] void* allocate_aligned(std::size_t size,
                                     std::size_t alignment) {
  count_allocation();
#if defined(_MSC_VER)
  if (void* memory = _aligned_malloc(size == 0 ? 1 : size, alignment)) {
    return memory;
  }
#else
  void* memory{};
  if (posix_memalign(&memory, alignment, size == 0 ? 1 : size) == 0) {
    return memory;
  }
#endif
  throw std::bad_alloc{};
}

} // namespace

void* operator new(std::size_t size) { return allocate(size); }
void* operator new[](std::size_t size) { return allocate(size); }
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }

void* operator new(std::size_t size, std::align_val_t alignment) {
  return allocate_aligned(size, static_cast<std::size_t>(alignment));
}

void* operator new[](std::size_t size, std::align_val_t alignment) {
  return allocate_aligned(size, static_cast<std::size_t>(alignment));
}

void operator delete(void* memory, std::align_val_t) noexcept {
#if defined(_MSC_VER)
  _aligned_free(memory);
#else
  std::free(memory);
#endif
}

void operator delete[](void* memory, std::align_val_t alignment) noexcept {
  operator delete(memory, alignment);
}

void operator delete(void* memory, std::size_t,
                     std::align_val_t alignment) noexcept {
  operator delete(memory, alignment);
}

void operator delete[](void* memory, std::size_t,
                       std::align_val_t alignment) noexcept {
  operator delete(memory, alignment);
}

namespace fexma::wal {

class WalTestAccess final {
public:
  [[nodiscard]] static std::span<const std::byte>
  block_at(const Wal& wal, std::uint64_t position) noexcept {
    const auto slot = static_cast<std::uint32_t>(position % wal.config_.capacity);
    return wal.storage_.block_at_slot(slot);
  }
};

} // namespace fexma::wal

using namespace fexma::wal;

namespace {

class PhysicalControlGuard final {
public:
  explicit PhysicalControlGuard(
      detail::PhysicalWalFileTestControl& control) noexcept {
    detail::set_physical_wal_file_test_control(&control);
  }

  ~PhysicalControlGuard() {
    detail::set_physical_wal_file_test_control(nullptr);
  }

  PhysicalControlGuard(const PhysicalControlGuard&) = delete;
  PhysicalControlGuard& operator=(const PhysicalControlGuard&) = delete;
};

[[nodiscard]] std::filesystem::path test_path(const char* name) {
  return std::filesystem::temp_directory_path() / name;
}

template <std::size_t N>
[[nodiscard]] std::array<std::byte, N> payload(std::uint64_t value) noexcept {
  std::array<std::byte, N> bytes{};
  for (std::size_t i = 0; i < bytes.size(); ++i) {
    const std::uint64_t mixed = value + (i * 131u);
    bytes[i] = static_cast<std::byte>(mixed & 0xffu);
  }
  return bytes;
}

template <std::size_t N>
[[nodiscard]] bool equal(const std::array<std::byte, N>& left,
                         const std::array<std::byte, N>& right) noexcept {
  return std::memcmp(left.data(), right.data(), N) == 0;
}

template <class T>
[[nodiscard]] bool read_exact(std::ifstream& file, T& value) {
  file.read(reinterpret_cast<char*>(&value), sizeof(value));
  return static_cast<bool>(file);
}

[[nodiscard]] bool invalid_configuration_is_rejected() {
  const auto path = test_path("fexma_wal_invalid_config.wal");
  std::filesystem::remove(path);

  Wal wal;
  return wal.open(path, {0, 1, 64}).status == OpenStatus::InvalidConfig &&
         wal.open(path, {8, 0, 64}).status == OpenStatus::InvalidConfig &&
         wal.open(path, {8, 1, 0}).status == OpenStatus::InvalidConfig &&
         wal.open(path, {8, 1, 24}).status == OpenStatus::InvalidConfig;
}

[[nodiscard]] bool storage_is_aligned_warmed_and_capacity_one_works() {
  const auto path = test_path("fexma_wal_storage.wal");
  const auto capacity_one_path = test_path("fexma_wal_capacity_one.wal");
  std::filesystem::remove(path);
  std::filesystem::remove(capacity_one_path);

  constexpr WalConfig config{33, 4096, 64};
  {
    Wal wal;
    if (!wal.open(path, config).ok()) {
      return false;
    }

    for (std::uint64_t position = 0; position < config.capacity; ++position) {
      const auto block = WalTestAccess::block_at(wal, position);
      if (block.size() != config.payload_size ||
          (reinterpret_cast<std::uintptr_t>(block.data()) %
           config.alignment) != 0) {
        return false;
      }
      for (const std::byte value : block) {
        if (value != std::byte{0}) {
          return false;
        }
      }
    }
    if (!wal.close().ok()) {
      return false;
    }
  }

  Wal wal;
  if (!wal.open(capacity_one_path, {33, 1, 64}).ok()) {
    return false;
  }
  const auto first = payload<33>(1);
  const auto second = payload<33>(2);
  std::array<std::byte, 33> output{};

  if (!wal.try_publish(first).ok() ||
      wal.try_publish(second).status != PublishStatus::Full ||
      wal.try_consume(output).status != ConsumeStatus::Empty ||
      !wal.advance_durable().ok() || !wal.try_consume(output).ok() ||
      !equal(first, output) || !wal.try_publish(second).ok() ||
      !wal.advance_durable().ok() || !wal.try_consume(output).ok() ||
      !equal(second, output) || !wal.close().ok()) {
    return false;
  }

  std::filesystem::remove(path);
  std::filesystem::remove(capacity_one_path);
  return true;
}

[[nodiscard]] bool hot_operations_do_not_allocate() {
  const auto path = test_path("fexma_wal_no_allocations.wal");
  std::filesystem::remove(path);

  Wal wal;
  if (!wal.open(path, {32, 4, 64}).ok()) {
    return false;
  }

  const auto input = payload<32>(10);
  std::array<std::byte, 32> output{};
  allocation_count.store(0, std::memory_order_relaxed);
  track_allocations.store(true, std::memory_order_release);

  const PublishResult published = wal.try_publish(input);
  const DurabilityResult durable = wal.advance_durable();
  const ConsumeResult consumed = wal.try_consume(output);

  track_allocations.store(false, std::memory_order_release);
  const bool no_allocations =
      allocation_count.load(std::memory_order_relaxed) == 0;
  const bool closed = wal.close().ok();
  std::filesystem::remove(path);
  return published.ok() && durable.ok() && consumed.ok() &&
         equal(input, output) && no_allocations && closed;
}

[[nodiscard]] bool fifo_wraparound_and_sequences_are_correct() {
  const auto path = test_path("fexma_wal_wraparound.wal");
  std::filesystem::remove(path);

  constexpr std::uint32_t capacity = 3;
  constexpr std::uint64_t message_count = 99;
  Wal wal;
  if (!wal.open(path, {16, capacity, 64}).ok()) {
    return false;
  }

  std::uint64_t next = 1;
  std::array<std::byte, 16> output{};
  while (next <= message_count) {
    const std::uint64_t batch_end =
        std::min<std::uint64_t>(message_count + 1u, next + capacity);
    for (std::uint64_t value = next; value < batch_end; ++value) {
      const PublishResult result = wal.try_publish(payload<16>(value));
      if (!result.ok() || result.sequence != value) {
        return false;
      }
    }
    if (batch_end <= message_count &&
        wal.try_publish(payload<16>(batch_end)).status != PublishStatus::Full) {
      return false;
    }

    const DurabilityResult durable = wal.advance_durable();
    if (!durable.ok() || durable.durable_frontier != batch_end - 1u) {
      return false;
    }

    for (std::uint64_t value = next; value < batch_end; ++value) {
      const ConsumeResult result = wal.try_consume(output);
      if (!result.ok() || result.sequence != value ||
          !equal(payload<16>(value), output)) {
        return false;
      }
    }
    if (wal.try_consume(output).status != ConsumeStatus::Empty) {
      return false;
    }
    next = batch_end;
  }

  const WalSnapshot snapshot = wal.snapshot();
  const bool valid = snapshot.tail == message_count &&
                     snapshot.durable == message_count &&
                     snapshot.head == message_count && wal.close().ok();
  std::filesystem::remove(path);
  return valid;
}

[[nodiscard]] bool payload_size_and_closed_states_are_reported() {
  const auto path = test_path("fexma_wal_statuses.wal");
  std::filesystem::remove(path);

  Wal wal;
  std::array<std::byte, 8> bytes{};
  if (wal.try_publish(bytes).status != PublishStatus::Closed ||
      wal.advance_durable().status != DurabilityStatus::Closed ||
      wal.try_consume(bytes).status != ConsumeStatus::Closed ||
      !wal.open(path, {16, 2, 64}).ok()) {
    return false;
  }

  const bool valid =
      wal.try_publish(bytes).status == PublishStatus::InvalidPayloadSize &&
      wal.try_consume(bytes).status == ConsumeStatus::InvalidPayloadSize &&
      wal.close().ok();
  std::filesystem::remove(path);
  return valid;
}

[[nodiscard]] bool durability_batches_sync_once() {
  const auto path = test_path("fexma_wal_batches.wal");
  std::filesystem::remove(path);

  Wal wal;
  if (!wal.open(path, {8, 8, 64}).ok()) {
    return false;
  }
  for (std::uint64_t value = 1; value <= 5; ++value) {
    if (!wal.try_publish(payload<8>(value)).ok()) {
      return false;
    }
  }

  detail::PhysicalWalFileTestControl control{};
  PhysicalControlGuard guard{control};

  const DurabilityResult zero = wal.advance_durable(0);
  const DurabilityResult first = wal.advance_durable(2);
  const DurabilityResult second = wal.advance_durable(2);
  const DurabilityResult third = wal.advance_durable(8);
  const DurabilityResult empty = wal.advance_durable();
  const WalSnapshot snapshot = wal.snapshot();

  const bool valid = zero.ok() && zero.records == 0 &&
                     first.ok() && first.records == 2 &&
                     first.durable_frontier == 2 && second.ok() &&
                     second.records == 2 && second.durable_frontier == 4 &&
                     third.ok() && third.records == 1 &&
                     third.durable_frontier == 5 && empty.ok() &&
                     empty.records == 0 && control.append_calls == 5 &&
                     control.sync_calls == 3 && snapshot.durable == 5 &&
                     snapshot.durable <= snapshot.head && wal.close().ok();
  std::filesystem::remove(path);
  return valid;
}

[[nodiscard]] bool append_failure_is_fail_closed_but_consumer_drains() {
  const auto path = test_path("fexma_wal_append_failure.wal");
  std::filesystem::remove(path);

  bool valid{};
  {
    Wal wal;
    if (!wal.open(path, {8, 4, 64}).ok() ||
        !wal.try_publish(payload<8>(1)).ok() ||
        !wal.advance_durable().ok()) {
      return false;
    }

    detail::PhysicalWalFileTestControl control{};
    control.fail_append_call = 0;
    PhysicalControlGuard guard{control};

    std::array<std::byte, 8> output{};
    const PublishResult second = wal.try_publish(payload<8>(2));
    const DurabilityResult failed = wal.advance_durable();
    const PublishResult after_failure = wal.try_publish(payload<8>(3));
    const DurabilityResult retry = wal.advance_durable();
    const ConsumeResult durable = wal.try_consume(output);
    const ConsumeResult hidden = wal.try_consume(output);
    const WalSnapshot snapshot = wal.snapshot();
    const CloseResult closed = wal.close();

    valid = second.ok() && failed.status == DurabilityStatus::IoError &&
            failed.durable_frontier == 1 &&
            after_failure.status == PublishStatus::IoError &&
            retry.status == DurabilityStatus::IoError && durable.ok() &&
            durable.sequence == 1 && equal(payload<8>(1), output) &&
            hidden.status == ConsumeStatus::Empty && snapshot.tail == 1 &&
            snapshot.durable == 1 && snapshot.head == 2 &&
            closed.status == CloseStatus::IoError;
  }
  std::filesystem::remove(path);
  return valid;
}

[[nodiscard]] bool sync_failure_is_fail_closed_but_consumer_drains() {
  const auto path = test_path("fexma_wal_sync_failure.wal");
  std::filesystem::remove(path);

  bool valid{};
  {
    Wal wal;
    if (!wal.open(path, {8, 4, 64}).ok() ||
        !wal.try_publish(payload<8>(1)).ok() ||
        !wal.advance_durable().ok()) {
      return false;
    }

    detail::PhysicalWalFileTestControl control{};
    control.fail_sync_call = 0;
    PhysicalControlGuard guard{control};

    std::array<std::byte, 8> output{};
    const PublishResult second = wal.try_publish(payload<8>(2));
    const DurabilityResult failed = wal.advance_durable();
    const PublishResult after_failure = wal.try_publish(payload<8>(3));
    const ConsumeResult durable = wal.try_consume(output);
    const ConsumeResult hidden = wal.try_consume(output);
    const WalSnapshot snapshot = wal.snapshot();
    const CloseResult closed = wal.close();

    valid = second.ok() && failed.status == DurabilityStatus::IoError &&
            failed.durable_frontier == 1 &&
            after_failure.status == PublishStatus::IoError && durable.ok() &&
            equal(payload<8>(1), output) && hidden.status == ConsumeStatus::Empty &&
            snapshot.tail == 1 && snapshot.durable == 1 &&
            snapshot.head == 2 && closed.status == CloseStatus::IoError;
  }
  std::filesystem::remove(path);
  return valid;
}

[[nodiscard]] bool physical_file_format_crc_and_padding_are_correct() {
  const auto path = test_path("fexma_wal_format.wal");
  std::filesystem::remove(path);

  constexpr WalConfig config{12, 4, 64};
  const std::array inputs{payload<12>(1), payload<12>(2), payload<12>(3)};
  {
    Wal wal;
    if (!wal.open(path, config).ok()) {
      return false;
    }
    for (const auto& input : inputs) {
      if (!wal.try_publish(input).ok()) {
        return false;
      }
    }
    if (!wal.advance_durable(2).ok() || !wal.advance_durable().ok() ||
        !wal.close().ok()) {
      return false;
    }
  }

  std::ifstream file(path, std::ios::binary);
  FileHeader file_header{};
  if (!read_exact(file, file_header) || file_header.magic != file_magic ||
      file_header.version != format_version ||
      file_header.header_size != sizeof(FileHeader) ||
      file_header.payload_size != config.payload_size ||
      file_header.alignment != config.alignment ||
      file_header.header_crc32 != file_header_crc32(file_header)) {
    return false;
  }

  const std::uint32_t padding = static_cast<std::uint32_t>(
      aligned_record_size(config) - sizeof(RecordHeader) - config.payload_size);
  for (std::size_t index = 0; index < inputs.size(); ++index) {
    RecordHeader header{};
    std::array<std::byte, config.payload_size> bytes{};
    std::array<std::byte, default_alignment> padding_bytes{};
    if (!read_exact(file, header)) {
      return false;
    }
    file.read(reinterpret_cast<char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    file.read(reinterpret_cast<char*>(padding_bytes.data()), padding);
    if (!file || header.magic != record_magic ||
        header.version != format_version ||
        header.header_size != sizeof(RecordHeader) ||
        header.sequence != index + 1u ||
        header.header_crc32 != record_header_crc32(header) ||
        header.payload_crc32 != crc32_bytes(bytes.data(), bytes.size()) ||
        !equal(inputs[index], bytes)) {
      return false;
    }
    for (std::uint32_t byte = 0; byte < padding; ++byte) {
      if (padding_bytes[byte] != std::byte{0}) {
        return false;
      }
    }
  }

  const bool valid = file.peek() == std::ifstream::traits_type::eof();
  file.close();
  std::filesystem::remove(path);
  return valid;
}

struct StressPayload {
  std::uint64_t sequence{};
  std::uint64_t check{};
};

static_assert(std::is_trivially_copyable_v<StressPayload>);

[[nodiscard]] std::array<std::byte, sizeof(StressPayload)>
stress_payload(std::uint64_t sequence) noexcept {
  const StressPayload value{sequence, sequence ^ 0x9e3779b97f4a7c15ull};
  std::array<std::byte, sizeof(value)> bytes{};
  std::memcpy(bytes.data(), &value, sizeof(value));
  return bytes;
}

[[nodiscard]] bool three_role_spsc_stress() {
  const auto path = test_path("fexma_wal_spsc_stress.wal");
  std::filesystem::remove(path);

  constexpr std::uint32_t capacity = 1024;
  constexpr std::uint32_t durability_batch = 512;
  constexpr std::uint64_t message_count = 50000;

  Wal wal;
  if (!wal.open(path, {sizeof(StressPayload), capacity, 64}).ok()) {
    return false;
  }

  std::atomic<bool> producer_done{false};
  std::atomic<bool> failed{false};

  std::thread producer([&] {
    for (std::uint64_t sequence = 1; sequence <= message_count; ++sequence) {
      const auto bytes = stress_payload(sequence);
      while (true) {
        const PublishResult result = wal.try_publish(bytes);
        if (result.ok()) {
          if (result.sequence != sequence) {
            failed.store(true, std::memory_order_release);
          }
          break;
        }
        if (result.status != PublishStatus::Full) {
          failed.store(true, std::memory_order_release);
          producer_done.store(true, std::memory_order_release);
          return;
        }
        std::this_thread::yield();
      }
    }
    producer_done.store(true, std::memory_order_release);
  });

  std::thread durability([&] {
    while (true) {
      if (failed.load(std::memory_order_acquire)) {
        return;
      }
      const WalSnapshot snapshot = wal.snapshot();
      const std::uint64_t pending = snapshot.head - snapshot.durable;
      if (snapshot.durable == message_count) {
        return;
      }
      if (pending >= durability_batch ||
          (producer_done.load(std::memory_order_acquire) && pending != 0)) {
        const DurabilityResult result =
            wal.advance_durable(durability_batch);
        if (!result.ok()) {
          failed.store(true, std::memory_order_release);
          return;
        }
      } else {
        std::this_thread::yield();
      }
    }
  });

  std::thread consumer([&] {
    std::array<std::byte, sizeof(StressPayload)> bytes{};
    for (std::uint64_t expected = 1; expected <= message_count;) {
      const ConsumeResult result = wal.try_consume(bytes);
      if (result.status == ConsumeStatus::Empty) {
        if (failed.load(std::memory_order_acquire)) {
          return;
        }
        std::this_thread::yield();
        continue;
      }
      if (!result.ok() || result.sequence != expected ||
          !equal(bytes, stress_payload(expected))) {
        failed.store(true, std::memory_order_release);
        return;
      }
      ++expected;
    }
  });

  producer.join();
  durability.join();
  consumer.join();

  const WalSnapshot snapshot = wal.snapshot();
  const bool valid = !failed.load(std::memory_order_acquire) &&
                     snapshot.tail == message_count &&
                     snapshot.durable == message_count &&
                     snapshot.head == message_count && wal.close().ok();
  std::filesystem::remove(path);
  return valid;
}

} // namespace

int main() {
  if (!invalid_configuration_is_rejected()) return 1;
  if (!storage_is_aligned_warmed_and_capacity_one_works()) return 2;
  if (!hot_operations_do_not_allocate()) return 3;
  if (!fifo_wraparound_and_sequences_are_correct()) return 4;
  if (!payload_size_and_closed_states_are_reported()) return 5;
  if (!durability_batches_sync_once()) return 6;
  if (!append_failure_is_fail_closed_but_consumer_drains()) return 7;
  if (!sync_failure_is_fail_closed_but_consumer_drains()) return 8;
  if (!physical_file_format_crc_and_padding_are_correct()) return 9;
  if (!three_role_spsc_stress()) return 10;
  return 0;
}
