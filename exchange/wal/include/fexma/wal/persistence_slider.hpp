#pragma once

/**
 * @file persistence_slider.hpp
 * @brief Static persistence stage over the common retained WAL string.
 */

#include <fexma/wal/persistence.hpp>
#include <fexma/wal/slider.hpp>

namespace fexma::wal {

class PersistenceBatchPublish final {
public:
  [[nodiscard]] PublishDecision
  after_process(PersistenceModule&, Position) const noexcept {
    return PublishDecision::Hold;
  }

  [[nodiscard]] PublishDecision
  after_range(PersistenceModule& module, Position begin,
              Position end) const noexcept {
    if (begin == end) return PublishDecision::Hold;
    return module.sync() ? PublishDecision::Publish : PublishDecision::Failed;
  }
};

using PersistenceSlider =
    Slider<WalCore, WalHeadProgress, Progress::Writer, PersistenceModule,
           BoundedRangeAcquire, PersistenceBatchPublish>;

static_assert(SliderModule<PersistenceModule>);

} // namespace fexma::wal
