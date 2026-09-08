#include <fexma/wal/format.hpp>

#include <limits>

namespace fexma::wal {
namespace {

[[nodiscard]] bool checked_add(std::size_t left, std::size_t right,
                               std::size_t& out) noexcept {
  if (left > std::numeric_limits<std::size_t>::max() - right) return false;
  out = left + right;
  return true;
}

[[nodiscard]] bool checked_mul(std::size_t left, std::size_t right,
                               std::size_t& out) noexcept {
  if (left != 0 && right > std::numeric_limits<std::size_t>::max() / left) {
    return false;
  }
  out = left * right;
  return true;
}

[[nodiscard]] bool checked_align_up(std::size_t value, std::size_t alignment,
                                    std::size_t& out) noexcept {
  std::size_t biased{};
  if (!checked_add(value, alignment - 1u, biased)) return false;
  out = (biased / alignment) * alignment;
  return true;
}

[[nodiscard]] bool valid_stream_kind(StreamKind kind) noexcept {
  switch (kind) {
  case StreamKind::Generic:
  case StreamKind::Command:
  case StreamKind::Event:
    return true;
  }
  return false;
}

} // namespace

bool valid_config(const WalConfig& config) noexcept {
  if (config.payload_size == 0 || config.capacity == 0 ||
      config.alignment < alignof(void*) || config.first_sequence == 0 ||
      !valid_stream_kind(config.stream_kind)) {
    return false;
  }
  if (config.stream_kind != StreamKind::Generic &&
      (config.stream_id == 0 || config.epoch_id == 0 ||
       config.manifest_id == 0)) {
    return false;
  }
  if ((config.alignment & (config.alignment - 1u)) != 0) return false;

  std::size_t storage_stride{};
  std::size_t storage_size{};
  if (!checked_align_up(config.payload_size, config.alignment,
                        storage_stride) ||
      !checked_mul(storage_stride, config.capacity, storage_size)) {
    return false;
  }
  return records_offset(config) != 0 && aligned_record_size(config) != 0;
}

} // namespace fexma::wal
