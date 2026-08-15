/**
 * @file test_wal_queue.cpp
 * @brief Contract tests for the fixed-payload durable queue.
 */

#include <fexma/wal/wal.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>

using namespace fexma::wal;

namespace {

[[nodiscard]] std::filesystem::path test_path(const char* name) {
  return std::filesystem::temp_directory_path() / name;
}

template <std::size_t N>
[[nodiscard]] std::array<std::byte, N> payload(std::uint8_t seed) noexcept {
  std::array<std::byte, N> bytes{};
  for (std::size_t i = 0; i < bytes.size(); ++i) {
    bytes[i] = static_cast<std::byte>(seed + static_cast<std::uint8_t>(i));
  }
  return bytes;
}

template <std::size_t N>
[[nodiscard]] bool equal(const std::array<std::byte, N>& left,
                         const std::array<std::byte, N>& right) noexcept {
  for (std::size_t i = 0; i < N; ++i) {
    if (left[i] != right[i]) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] bool exposes_only_the_durable_range() {
  const auto path = test_path("fexma_wal_frontier.wal");
  std::filesystem::remove(path);

  Wal wal;
  if (!wal.open(path, {16, 4, 64}).ok()) {
    return false;
  }
  if (wal.read_cursor() != 0 || wal.durable_cursor() != 0 ||
      wal.write_cursor() != 0) {
    return false;
  }

  const auto input = payload<16>(7);
  std::array<std::byte, 16> output{};
  const PushResult pushed = wal.try_push(input);
  if (!pushed.ok() || pushed.sequence != 1 || wal.read_cursor() != 0 ||
      wal.durable_cursor() != 0 || wal.write_cursor() != 1) {
    return false;
  }

  const PopResult before_durable = wal.try_pop(output);
  if (before_durable.status != PopStatus::Empty) {
    return false;
  }

  const DurabilityResult persisted = wal.advance_durable();
  if (!persisted.ok() || persisted.durable_cursor != 1 ||
      persisted.records != 1 || wal.read_cursor() != 0 ||
      wal.durable_cursor() != 1 || wal.write_cursor() != 1) {
    return false;
  }

  const PopResult popped = wal.try_pop(output);
  const CloseResult closed = wal.close();

  std::filesystem::remove(path);
  return popped.ok() && popped.sequence == 1 && equal(input, output) &&
         wal.read_cursor() == 1 && wal.durable_cursor() == 1 &&
         wal.write_cursor() == 1 && closed.ok();
}

[[nodiscard]] bool applies_backpressure_and_reuses_read_slot() {
  const auto path = test_path("fexma_wal_capacity.wal");
  std::filesystem::remove(path);

  Wal wal;
  if (!wal.open(path, {8, 2, 64}).ok()) {
    return false;
  }

  const auto first = payload<8>(1);
  const auto second = payload<8>(20);
  const auto third = payload<8>(40);
  if (!wal.try_push(first).ok() || !wal.try_push(second).ok() ||
      wal.try_push(third).status != PushStatus::Full ||
      !wal.advance_durable(1).ok()) {
    return false;
  }

  std::array<std::byte, 8> output{};
  if (!wal.try_pop(output).ok() || !equal(first, output)) {
    return false;
  }

  const PushResult reused = wal.try_push(third);
  const DurabilityResult persisted = wal.advance_durable();
  const CloseResult closed = wal.close();
  std::filesystem::remove(path);

  return reused.ok() && reused.sequence == 3 && persisted.ok() &&
         persisted.durable_cursor == 3 && persisted.records == 2 &&
         closed.ok();
}

[[nodiscard]] bool rejects_wrong_payload_size_and_pending_close() {
  const auto path = test_path("fexma_wal_contract_errors.wal");
  std::filesystem::remove(path);

  Wal wal;
  if (!wal.open(path, {16, 2, 64}).ok()) {
    return false;
  }

  std::array<std::byte, 8> wrong_size{};
  const auto input = payload<16>(3);
  const PushResult wrong_push = wal.try_push(wrong_size);
  const PopResult wrong_pop = wal.try_pop(wrong_size);
  const PushResult pushed = wal.try_push(input);
  const CloseResult pending = wal.close();
  const DurabilityResult persisted = wal.advance_durable();
  const CloseResult closed = wal.close();

  std::filesystem::remove(path);
  return wrong_push.status == PushStatus::InvalidPayloadSize &&
         wrong_pop.status == PopStatus::InvalidPayloadSize && pushed.ok() &&
         pending.status == CloseStatus::PendingDurability &&
         persisted.ok() && closed.ok();
}

[[nodiscard]] bool writes_durable_records_in_physical_order() {
  const auto path = test_path("fexma_wal_physical_order.wal");
  std::filesystem::remove(path);

  const WalConfig config{12, 4, 64};
  Wal wal;
  if (!wal.open(path, config).ok()) {
    return false;
  }

  const auto first = payload<12>(10);
  const auto second = payload<12>(30);
  if (!wal.try_push(first).ok() || !wal.try_push(second).ok() ||
      !wal.advance_durable(1).ok()) {
    return false;
  }

  const std::uint64_t one_record_size =
      sizeof(FileHeader) + aligned_record_size(config);
  if (std::filesystem::file_size(path) != one_record_size ||
      !wal.advance_durable().ok() || !wal.close().ok()) {
    return false;
  }

  std::ifstream file(path, std::ios::binary);
  FileHeader file_header{};
  RecordHeader first_header{};
  RecordHeader second_header{};
  std::array<std::byte, 12> first_bytes{};
  std::array<std::byte, 12> second_bytes{};

  file.read(reinterpret_cast<char*>(&file_header), sizeof(file_header));
  file.read(reinterpret_cast<char*>(&first_header), sizeof(first_header));
  file.read(reinterpret_cast<char*>(first_bytes.data()),
            static_cast<std::streamsize>(first_bytes.size()));
  file.seekg(static_cast<std::streamoff>(
                 sizeof(FileHeader) + aligned_record_size(config)),
             std::ios::beg);
  file.read(reinterpret_cast<char*>(&second_header), sizeof(second_header));
  file.read(reinterpret_cast<char*>(second_bytes.data()),
            static_cast<std::streamsize>(second_bytes.size()));

  const bool valid = file_header.magic == file_magic &&
                     file_header.payload_size == config.payload_size &&
                     file_header.alignment == config.alignment &&
                     file_header.header_crc32 == file_header_crc32(file_header) &&
                     first_header.sequence == 1 && second_header.sequence == 2 &&
                     first_header.header_crc32 ==
                         record_header_crc32(first_header) &&
                     second_header.header_crc32 ==
                         record_header_crc32(second_header) &&
                     first_header.payload_crc32 ==
                         crc32_bytes(first_bytes.data(), first_bytes.size()) &&
                     second_header.payload_crc32 ==
                         crc32_bytes(second_bytes.data(), second_bytes.size()) &&
                     equal(first, first_bytes) && equal(second, second_bytes);

  file.close();
  std::filesystem::remove(path);
  return valid;
}

} // namespace

int main() {
  if (!exposes_only_the_durable_range()) {
    return 1;
  }
  if (!applies_backpressure_and_reuses_read_slot()) {
    return 2;
  }
  if (!rejects_wrong_payload_size_and_pending_close()) {
    return 3;
  }
  if (!writes_durable_records_in_physical_order()) {
    return 4;
  }
  return 0;
}
