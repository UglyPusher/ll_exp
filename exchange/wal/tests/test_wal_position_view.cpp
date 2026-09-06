/**
 * @file test_wal_position_view.cpp
 * @brief Absolute-position borrowed-view contract tests.
 */
#include <fexma/wal/wal.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <new>
#include <thread>
#include <type_traits>
#include <utility>

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

using namespace fexma::wal;

static_assert(noexcept(std::declval<const Wal&>().try_view(Position{})));
static_assert(std::is_same_v<decltype(RecordView{}.payload),
                             std::span<const std::byte>>);
static_assert(!std::is_assignable_v<decltype(RecordView{}.payload[0]),
                                   std::byte>);

namespace {

using Payload = std::array<std::byte, 16>;

[[nodiscard]] Payload payload(Position position) noexcept {
  Payload bytes{};
  for (std::size_t index = 0; index < bytes.size(); ++index) {
    bytes[index] = static_cast<std::byte>((position + index * 17u) & 0xffu);
  }
  return bytes;
}

[[nodiscard]] bool matches(const AccessResult& result, Position position,
                            std::uint64_t first_sequence) noexcept {
  const Payload expected = payload(position);
  return result.ok() && result.record.position == position &&
         result.record.sequence == first_sequence + position &&
         std::equal(result.record.payload.begin(), result.record.payload.end(),
                    expected.begin(), expected.end());
}

[[nodiscard]] bool rejected(const Wal& wal, Position position,
                             ViewStatus status) noexcept {
  const AccessResult result = wal.try_view(position);
  return !result.ok() && result.status == status &&
         result.record.payload.empty() && result.record.position == 0 &&
         result.record.sequence == 0;
}

[[nodiscard]] bool boundaries_and_retained_lifetime() {
  const auto path = std::filesystem::temp_directory_path() /
                    "fexma_wal_view_boundaries.wal";
  std::filesystem::remove(path);
  Wal wal;
  const Wal& reader = wal;
  constexpr Position maximum = std::numeric_limits<Position>::max();
  if (!rejected(reader, 0, ViewStatus::Closed) ||
      !rejected(reader, maximum, ViewStatus::Closed) ||
      !wal.open(path, {16, 4, 64, 0, StreamKind::Generic, 0, 0, 101}).ok() ||
      !rejected(reader, 0, ViewStatus::Unpublished)) {
    return false;
  }
  for (Position position = 0; position < 3; ++position) {
    if (!wal.try_publish(payload(position)).ok()) return false;
  }

  {
    const auto first = reader.try_view(0);
    const auto middle = reader.try_view(1);
    const auto last = reader.try_view(2);
    if (!matches(first, 0, 101) || !matches(middle, 1, 101) ||
        !matches(last, 2, 101) ||
        !rejected(reader, 3, ViewStatus::Unpublished) ||
        !rejected(reader, maximum, ViewStatus::Unpublished)) {
      return false;
    }
    const WalSnapshot unchanged = wal.snapshot();
    if (unchanged.tail != 0 || unchanged.durable != 0 || unchanged.head != 3) {
      return false;
    }
    Payload out{};
    if (wal.try_consume(out).status != ConsumeStatus::Empty ||
        !wal.try_publish(payload(3)).ok() || !wal.advance_durable().ok() ||
        wal.try_publish(payload(4)).status != PublishStatus::Full ||
        !matches(first, 0, 101) || !matches(middle, 1, 101) ||
        !matches(last, 2, 101)) {
      return false;
    }
    const auto again = reader.try_view(0);
    if (again.record.payload.data() != first.record.payload.data()) return false;
  } // Retire views before try_consume advances reclamation.

  Payload out{};
  for (Position position = 0; position < 4; ++position) {
    if (!wal.try_consume(out).ok() || out != payload(position) ||
        !rejected(reader, position, ViewStatus::Reclaimed)) {
      return false;
    }
  }
  if (!rejected(reader, 4, ViewStatus::Unpublished) || !wal.close().ok() ||
      !rejected(reader, 0, ViewStatus::Closed)) {
    return false;
  }
  std::filesystem::remove(path);
  return true;
}

[[nodiscard]] bool wrapped_positions_do_not_alias() {
  const auto path = std::filesystem::temp_directory_path() /
                    "fexma_wal_view_wraparound.wal";
  for (const std::uint32_t capacity : {1u, 3u}) {
    std::filesystem::remove(path);
    Wal wal;
    if (!wal.open(path, {16, capacity, 64}).ok()) return false;
    std::array<std::uintptr_t, 3> addresses{};
    for (Position start = 0; start < 99; start += capacity) {
      for (Position offset = 0; offset < capacity; ++offset) {
        if (!wal.try_publish(payload(start + offset)).ok()) return false;
      }
      for (Position offset = 0; offset < capacity; ++offset) {
        const Position position = start + offset;
        const auto view = wal.try_view(position);
        if (!matches(view, position, 1)) return false;
        const auto address =
            reinterpret_cast<std::uintptr_t>(view.record.payload.data());
        if (start == 0) {
          addresses[static_cast<std::size_t>(offset)] = address;
        } else if (address != addresses[static_cast<std::size_t>(offset)] ||
                   !rejected(wal, position - capacity, ViewStatus::Reclaimed)) {
          return false;
        }
        if (!rejected(wal, position + capacity, ViewStatus::Unpublished)) {
          return false;
        }
      }
      if (!wal.advance_durable().ok()) return false;
      Payload out{};
      for (Position offset = 0; offset < capacity; ++offset) {
        if (!wal.try_consume(out).ok() || out != payload(start + offset)) {
          return false;
        }
      }
    }
    if (!wal.close().ok()) return false;
    std::filesystem::remove(path);
  }
  return true;
}

[[nodiscard]] bool two_readers_observe_publication() {
  const auto path = std::filesystem::temp_directory_path() /
                    "fexma_wal_view_two_readers.wal";
  std::filesystem::remove(path);
  Wal wal;
  constexpr std::uint32_t count = 64;
  if (!wal.open(path, {16, count, 64}).ok()) return false;
  const Wal& reader = wal;
  std::array<std::array<std::uintptr_t, count>, 2> addresses{};
  std::atomic<bool> failed{false};
  auto read = [&](std::size_t reader_index) {
    for (Position position = 0; position < count; ++position) {
      while (!failed.load(std::memory_order_relaxed)) {
        const auto view = reader.try_view(position);
        if (view.status == ViewStatus::Unpublished) {
          std::this_thread::yield();
          continue;
        }
        if (!matches(view, position, 1)) {
          failed.store(true, std::memory_order_relaxed);
          return;
        }
        addresses[reader_index][static_cast<std::size_t>(position)] =
            reinterpret_cast<std::uintptr_t>(view.record.payload.data());
        break;
      }
    }
  };
  std::thread first(read, 0);
  std::thread second(read, 1);
  for (Position position = 0; position < count; ++position) {
    if (!wal.try_publish(payload(position)).ok()) {
      failed.store(true, std::memory_order_relaxed);
      break;
    }
  }
  first.join();
  second.join();
  // No reclamation occurs until both readers have finished using every view.
  if (failed.load(std::memory_order_relaxed) || addresses[0] != addresses[1] ||
      !wal.advance_durable().ok()) {
    return false;
  }
  Payload out{};
  for (Position position = 0; position < count; ++position) {
    if (!wal.try_consume(out).ok()) return false;
  }
  const bool closed = wal.close().ok();
  std::filesystem::remove(path);
  return closed;
}

[[nodiscard]] bool sequence_limit_and_allocation_free_access() {
  const auto path = std::filesystem::temp_directory_path() /
                    "fexma_wal_view_sequence_limit.wal";
  std::filesystem::remove(path);
  Wal wal;
  constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
  if (!wal.open(path, {16, 2, 64, 0, StreamKind::Generic, 0, 0, maximum}).ok() ||
      !wal.try_publish(payload(0)).ok() ||
      wal.try_publish(payload(1)).status != PublishStatus::SequenceExhausted) {
    return false;
  }
  bool valid{};
  {
    allocation_count.store(0, std::memory_order_relaxed);
    track_allocations.store(true, std::memory_order_release);
    const auto view = wal.try_view(0);
    valid = matches(view, 0, maximum) &&
            rejected(wal, 1, ViewStatus::Unpublished) &&
            rejected(wal, maximum, ViewStatus::Unpublished);
    track_allocations.store(false, std::memory_order_release);
  }
  valid = valid && allocation_count.load(std::memory_order_relaxed) == 0;
  Payload out{};
  if (!wal.advance_durable().ok() || !wal.try_consume(out).ok()) return false;
  allocation_count.store(0, std::memory_order_relaxed);
  track_allocations.store(true, std::memory_order_release);
  const bool reclaimed = rejected(wal, 0, ViewStatus::Reclaimed);
  track_allocations.store(false, std::memory_order_release);
  valid = valid && reclaimed && allocation_count.load(std::memory_order_relaxed) == 0;
  if (wal.close().status != CloseStatus::SequenceExhausted) return false;
  allocation_count.store(0, std::memory_order_relaxed);
  track_allocations.store(true, std::memory_order_release);
  const bool closed = rejected(wal, 0, ViewStatus::Closed);
  track_allocations.store(false, std::memory_order_release);
  valid = valid && closed && allocation_count.load(std::memory_order_relaxed) == 0;
  std::filesystem::remove(path);
  return valid;
}

} // namespace

int main() {
  if (!boundaries_and_retained_lifetime()) return 1;
  if (!wrapped_positions_do_not_alias()) return 2;
  if (!two_readers_observe_publication()) return 3;
  if (!sequence_limit_and_allocation_free_access()) return 4;
  return 0;
}
