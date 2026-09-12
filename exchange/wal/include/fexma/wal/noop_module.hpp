#pragma once

/**
 * @file noop_module.hpp
 * @brief Trivial successful module for proving WAL slider composition.
 */

#include <fexma/wal/record_tape_types.hpp>

namespace fexma::wal {

class NoOpModule final {
public:
  [[nodiscard]] bool process(const RecordView&) noexcept { return true; }
};

} // namespace fexma::wal
