#pragma once

namespace fexma::snapshot_demo::detail {

struct SnapshotSinkTestControl {
  bool fail_open{};
  bool fail_write{};
  bool fail_flush{};
  bool fail_sync{};
  bool fail_publication{};
};

void set_snapshot_sink_test_control(SnapshotSinkTestControl* control) noexcept;

} // namespace fexma::snapshot_demo::detail
