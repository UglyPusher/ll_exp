#pragma once

/**
 * @file physical_wal_adapter.hpp
 * @brief Compile-time selection point for the physical WAL implementation.
 *
 * The selected header defines fexma::wal::detail::PhysicalWalAdapter and
 * PhysicalWalReaderAdapter with the direct write/read contracts used by the
 * runtime WAL and cold-path reader. Selecting a filesystem, direct-NVMe, or
 * another hardware-specific implementation means changing this include and
 * the corresponding build source, then rebuilding. No runtime dispatch,
 * virtual interface, or CRTP is used.
 */

#include "physical_wal_file.hpp"
