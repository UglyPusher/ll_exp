#pragma once

/**
 * @file physical_wal_adapter.hpp
 * @brief Compile-time selection point for the physical WAL implementation.
 *
 * The selected header defines fexma::wal::detail::PhysicalWalAdapter with the
 * direct create/append_record/sync/close contract used by Wal. Selecting a
 * filesystem, direct-NVMe, or another hardware-specific implementation means
 * changing this include and the corresponding build source, then rebuilding.
 * No runtime dispatch, virtual interface, or CRTP is used.
 */

#include "physical_wal_file.hpp"
