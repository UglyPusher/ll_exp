/**
 * @file test_large_capture.cpp
 * @brief Large synthetic state/capture stress checks for Demo 006.
 */

#include <fexma/wal/slider.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <span>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

using namespace fexma;

namespace {

using Clock = std::chrono::steady_clock;

constexpr std::size_t mib = 1024u * 1024u;
constexpr std::size_t io_chunk_size = 4u * mib;

class SyntheticSource final {
public:
  [[nodiscard]] wal::AccessResult
  try_view(wal::Position position) const noexcept {
    if (position >= payloads_.size()) {
      return {wal::ViewStatus::Unpublished, {}};
    }
    return {wal::ViewStatus::Ok,
            {position, 1000u + position,
             std::span<const std::byte>{payloads_[position]}}};
  }

private:
  const std::array<std::array<std::byte, 1>, 2> payloads_{
      std::array<std::byte, 1>{std::byte{0x53}},
      std::array<std::byte, 1>{std::byte{0x44}}};
};

struct SyntheticCapture {
  std::uint64_t generation_id{};
  wal::Position record_position{};
  wal::Position processed_end{};
  std::uint64_t sequence{};
  std::span<const std::byte> state{};
};

class SyntheticLargeStateModule final {
public:
  explicit SyntheticLargeStateModule(std::size_t state_size)
      : state_(state_size), capture_(state_size) {
    for (std::size_t index = 0; index < state_.size(); ++index) {
      state_[index] = static_cast<std::byte>(
          (index * 131u + index / 251u + 0x5au) & 0xffu);
    }
  }

  [[nodiscard]] bool process(const wal::RecordView& record) noexcept {
    if (failed_ || record.position != processed_end_) {
      failed_ = true;
      return false;
    }
    apply_transition(record.position, record.sequence, record.payload.front());
    ++processed_end_;

    if (record.position == 0) {
      if (capture_active_) return false;
      const auto started = Clock::now();
      std::memcpy(capture_.data(), state_.data(), state_.size());
      capture_duration_ns_ = elapsed_ns(started);
      capture_generation_ = record.position;
      capture_position_ = record.position;
      capture_processed_end_ = processed_end_;
      capture_sequence_ = record.sequence;
      capture_active_ = true;
    }
    return true;
  }

  [[nodiscard]] SyntheticCapture pending_capture() const noexcept {
    if (!capture_active_) return {};
    return {capture_generation_, capture_position_, capture_processed_end_,
            capture_sequence_, std::span<const std::byte>{capture_}};
  }

  [[nodiscard]] std::span<const std::byte> state() const noexcept {
    return state_;
  }

  [[nodiscard]] std::uint64_t capture_duration_ns() const noexcept {
    return capture_duration_ns_;
  }

  [[nodiscard]] wal::Position processed_end() const noexcept {
    return processed_end_;
  }

  [[nodiscard]] bool failed() const noexcept { return failed_; }

  [[nodiscard]] bool release_capture(std::uint64_t generation_id,
                                     wal::Position processed_end) noexcept {
    if (!capture_active_ || capture_generation_ != generation_id ||
        capture_processed_end_ != processed_end) {
      return false;
    }
    capture_active_ = false;
    return true;
  }

private:
  [[nodiscard]] static std::uint64_t
  elapsed_ns(Clock::time_point started) noexcept {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() -
                                                             started)
            .count());
  }

  void apply_transition(wal::Position position, std::uint64_t sequence,
                        std::byte payload) noexcept {
    const std::uint8_t salt = static_cast<std::uint8_t>(
        position ^ sequence ^ std::to_integer<std::uint8_t>(payload));
    for (std::size_t offset = 0; offset < state_.size(); offset += 4096u) {
      state_[offset] ^= static_cast<std::byte>(salt + offset / 4096u);
    }
    state_.back() ^= static_cast<std::byte>(salt ^ 0xa5u);
  }

  std::vector<std::byte> state_;
  std::vector<std::byte> capture_;
  wal::Position processed_end_{};
  std::uint64_t capture_generation_{};
  wal::Position capture_position_{};
  wal::Position capture_processed_end_{};
  std::uint64_t capture_sequence_{};
  std::uint64_t capture_duration_ns_{};
  bool capture_active_{};
  bool failed_{};
};

[[nodiscard]] std::FILE*
open_file(const std::filesystem::path& path, const wchar_t* windows_mode,
          const char* posix_mode) noexcept {
#if defined(_WIN32)
  static_cast<void>(posix_mode);
  std::FILE* file = nullptr;
  return ::_wfopen_s(&file, path.c_str(), windows_mode) == 0 ? file : nullptr;
#else
  static_cast<void>(windows_mode);
  return std::fopen(path.c_str(), posix_mode);
#endif
}

[[nodiscard]] bool sync_file(std::FILE* file) noexcept {
  if (std::fflush(file) != 0) return false;
#if defined(_WIN32)
  return ::_commit(::_fileno(file)) == 0;
#else
  return ::fsync(::fileno(file)) == 0;
#endif
}

[[nodiscard]] bool publish_capture(const std::filesystem::path& staging,
                                   const std::filesystem::path& published,
                                   std::span<const std::byte> capture,
                                   std::uint64_t& write_ns) {
  std::FILE* file = open_file(staging, L"wb", "wb");
  if (file == nullptr) return false;

  const auto started = Clock::now();
  std::size_t offset = 0;
  while (offset < capture.size()) {
    const std::size_t count =
        std::min(io_chunk_size, capture.size() - offset);
    if (std::fwrite(capture.data() + offset, 1u, count, file) != count) {
      std::fclose(file);
      return false;
    }
    offset += count;
  }
  const bool synced = sync_file(file);
  const bool closed = std::fclose(file) == 0;
  if (!synced || !closed) return false;
  write_ns = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() -
                                                           started)
          .count());

  std::error_code error;
  std::filesystem::rename(staging, published, error);
  return !error;
}

[[nodiscard]] bool verify_capture(const std::filesystem::path& published,
                                  std::span<const std::byte> expected,
                                  std::uint64_t& read_ns) {
  std::error_code error;
  if (std::filesystem::file_size(published, error) != expected.size() || error) {
    return false;
  }
  std::FILE* file = open_file(published, L"rb", "rb");
  if (file == nullptr) return false;

  const auto started = Clock::now();
  std::vector<std::byte> buffer(io_chunk_size);
  std::size_t offset = 0;
  bool valid = true;
  while (offset < expected.size()) {
    const std::size_t count =
        std::min(buffer.size(), expected.size() - offset);
    if (std::fread(buffer.data(), 1u, count, file) != count ||
        std::memcmp(buffer.data(), expected.data() + offset, count) != 0) {
      valid = false;
      break;
    }
    offset += count;
  }
  if (valid) valid = std::fgetc(file) == EOF;
  const bool closed = std::fclose(file) == 0;
  valid = valid && closed;
  read_ns = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() -
                                                           started)
          .count());
  return valid;
}

[[nodiscard]] bool run_size(std::size_t state_size_mib) {
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() /
      ("fexma_snapshot_demo_large_capture_" +
       std::to_string(state_size_mib));
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);
  const std::filesystem::path staging = root / "capture.pending";
  const std::filesystem::path published = root / "capture.published";

  SyntheticSource source;
  wal::Progress upstream(1);
  wal::Progress frontier;
  SyntheticLargeStateModule module(state_size_mib * mib);
  wal::Slider slider(source, upstream.reader(), frontier.writer(), module);

  const wal::SliderResult snapshot_result = slider.process_available();
  const SyntheticCapture capture = module.pending_capture();
  if (snapshot_result.status != wal::SliderStatus::Processed ||
      snapshot_result.processed_count != 1 || slider.current() != 1 ||
      frontier.reader().acquire() != 1 || module.failed() ||
      module.processed_end() != 1 ||
      capture.state.size() != state_size_mib * mib ||
      capture.generation_id != 0 || capture.record_position != 0 ||
      capture.processed_end != 1 || capture.sequence != 1000 ||
      std::memcmp(module.state().data(), capture.state.data(),
                  capture.state.size()) != 0) {
    std::filesystem::remove_all(root);
    return false;
  }

  const std::byte first_capture_byte = capture.state.front();
  const std::byte last_capture_byte = capture.state.back();
  if (!upstream.writer().publish(2)) {
    std::filesystem::remove_all(root);
    return false;
  }
  const wal::SliderResult continued = slider.process_available();
  if (continued.status != wal::SliderStatus::Processed ||
      continued.processed_count != 1 || slider.current() != 2 ||
      frontier.reader().acquire() != 2 || module.failed() ||
      module.processed_end() != 2 ||
      module.pending_capture().state.data() != capture.state.data() ||
      capture.state.front() != first_capture_byte ||
      capture.state.back() != last_capture_byte ||
      std::memcmp(module.state().data(), capture.state.data(),
                  capture.state.size()) == 0) {
    std::filesystem::remove_all(root);
    return false;
  }

  std::uint64_t write_ns = 0;
  std::uint64_t read_ns = 0;
  const bool persisted =
      publish_capture(staging, published, capture.state, write_ns) &&
      !std::filesystem::exists(staging) &&
      verify_capture(published, capture.state, read_ns) &&
      module.release_capture(capture.generation_id, capture.processed_end) &&
      module.pending_capture().state.empty();

  std::cout << "large-capture size_mib=" << state_size_mib
            << " capture_ms=" << module.capture_duration_ns() / 1'000'000u
            << " write_sync_ms=" << write_ns / 1'000'000u
            << " read_verify_ms=" << read_ns / 1'000'000u << '\n';
  std::filesystem::remove_all(root);
  return persisted;
}

} // namespace

int main() {
  try {
    for (const std::size_t size_mib : {1u, 10u, 100u, 500u}) {
      if (!run_size(size_mib)) return 1;
    }
  } catch (...) {
    return 2;
  }
  return 0;
}
