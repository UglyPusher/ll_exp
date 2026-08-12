/**
 * @file test_matcher.cpp
 * @brief Contract tests for the minimal matcher sample.
 */
#include <fexma/matcher/matcher.hpp>

#include <array>
#include <cstddef>
#include <cstdlib>

using namespace fexma::matcher;

namespace {

class ShutdownCommandReader {
public:
  [[nodiscard]] CommandReadResult read_next() noexcept {
    return {CommandReadStatus::Ok, {CommandType::Shutdown, {}}};
  }
};

class CollectingEventWriter {
public:
  [[nodiscard]] PublishResult publish(const Event& event) noexcept {
    if (fail_after != no_failure && event_count >= fail_after) {
      return {PublishStatus::Fatal};
    }
    if (event_count == events.size()) {
      return {PublishStatus::Fatal};
    }
    events[event_count++] = event;
    return {PublishStatus::Ok};
  }

  static constexpr std::size_t no_failure =
      static_cast<std::size_t>(-1);

  std::array<Event, 64> events{};
  std::size_t event_count{};
  std::size_t fail_after{no_failure};
};

[[nodiscard]] Command new_limit(OrderId id, OwnerId owner_id, Side side,
                                PriceTick price, Quantity quantity) noexcept {
  return {CommandType::NewLimit, {id, owner_id, side, price, quantity}};
}

[[nodiscard]] bool event_type_is(const CollectingEventWriter& writer,
                                 std::size_t index,
                                 EventType type) noexcept {
  return index < writer.event_count && writer.events[index].type == type;
}

[[nodiscard]] bool rests_non_crossing_limit() {
  ShutdownCommandReader reader;
  CollectingEventWriter writer;
  Matcher matcher(reader, writer, OrderBookConfig{100, 200, 8});

  const ProcessResult processed =
      matcher.process(new_limit(1, 101, Side::Bid, 150, 10));
  const auto best_bid = matcher.book().best(Side::Bid);

  return processed.status == ProcessStatus::Continue && best_bid &&
         best_bid->id == 1 && best_bid->remaining == 10 &&
         writer.event_count == 2 &&
         event_type_is(writer, 0, EventType::OrderAccepted) &&
         event_type_is(writer, 1, EventType::OrderRested);
}

[[nodiscard]] bool matches_fifo_at_resting_price() {
  ShutdownCommandReader reader;
  CollectingEventWriter writer;
  Matcher matcher(reader, writer, OrderBookConfig{100, 200, 8});

  (void)matcher.process(new_limit(1, 101, Side::Ask, 105, 4));
  (void)matcher.process(new_limit(2, 202, Side::Ask, 105, 6));
  const ProcessResult processed =
      matcher.process(new_limit(3, 303, Side::Bid, 106, 7));

  const auto best_ask = matcher.book().best(Side::Ask);
  if (processed.status != ProcessStatus::Continue || !best_ask ||
      best_ask->id != 2 || best_ask->remaining != 3) {
    return false;
  }

  std::size_t trade_count = 0;
  bool first_trade_ok = false;
  bool second_trade_ok = false;
  for (std::size_t i = 0; i < writer.event_count; ++i) {
    const Event& event = writer.events[i];
    if (event.type != EventType::Trade) {
      continue;
    }
    if (trade_count == 0) {
      first_trade_ok = event.trade.maker_order_id == 1 &&
                       event.trade.price == 105 &&
                       event.trade.quantity == 4;
    } else if (trade_count == 1) {
      second_trade_ok = event.trade.maker_order_id == 2 &&
                        event.trade.price == 105 &&
                        event.trade.quantity == 3;
    }
    ++trade_count;
  }

  return trade_count == 2 && first_trade_ok && second_trade_ok &&
         matcher.book().validate_invariants();
}

[[nodiscard]] bool rejects_zero_quantity() {
  ShutdownCommandReader reader;
  CollectingEventWriter writer;
  Matcher matcher(reader, writer, OrderBookConfig{100, 200, 8});

  const ProcessResult processed =
      matcher.process(new_limit(1, 101, Side::Bid, 150, 0));

  return processed.status == ProcessStatus::Continue &&
         writer.event_count == 1 &&
         event_type_is(writer, 0, EventType::OrderRejected) &&
         writer.events[0].rejected.reason == RejectReason::InvalidQuantity &&
         !matcher.book().best(Side::Bid);
}

[[nodiscard]] bool fatal_publish_stops_matcher() {
  ShutdownCommandReader reader;
  CollectingEventWriter writer;
  writer.fail_after = 1;
  Matcher matcher(reader, writer, OrderBookConfig{100, 200, 8});

  const ProcessResult processed =
      matcher.process(new_limit(1, 101, Side::Bid, 150, 10));
  const RunResult rerun = matcher.run();

  return processed.status == ProcessStatus::Fatal &&
         processed.fatal_reason == FatalReason::EventWriterFatal &&
         matcher.fatal() &&
         matcher.fatal_reason() == FatalReason::EventWriterFatal &&
         rerun.status == RunStatus::Fatal &&
         rerun.fatal_reason == FatalReason::EventWriterFatal;
}

[[nodiscard]] bool accepts_strictly_increasing_order_ids() {
  ShutdownCommandReader reader;
  CollectingEventWriter writer;
  Matcher matcher(reader, writer, OrderBookConfig{100, 200, 8});

  const ProcessResult first =
      matcher.process(new_limit(1, 101, Side::Bid, 150, 1));
  const ProcessResult second =
      matcher.process(new_limit(2, 102, Side::Bid, 151, 1));
  const ProcessResult third =
      matcher.process(new_limit(3, 103, Side::Bid, 152, 1));

  return first.status == ProcessStatus::Continue &&
         second.status == ProcessStatus::Continue &&
         third.status == ProcessStatus::Continue && !matcher.fatal() &&
         matcher.book().validate_invariants();
}

[[nodiscard]] bool repeated_order_id_is_fatal() {
  ShutdownCommandReader reader;
  CollectingEventWriter writer;
  Matcher matcher(reader, writer, OrderBookConfig{100, 200, 8});

  (void)matcher.process(new_limit(1, 101, Side::Bid, 150, 1));
  (void)matcher.process(new_limit(2, 102, Side::Bid, 151, 1));
  const std::size_t event_count_before = writer.event_count;
  const ProcessResult repeated =
      matcher.process(new_limit(2, 202, Side::Bid, 152, 1));

  return repeated.status == ProcessStatus::Fatal &&
         repeated.fatal_reason == FatalReason::NonMonotonicOrderId &&
         matcher.fatal() &&
         matcher.fatal_reason() == FatalReason::NonMonotonicOrderId &&
         writer.event_count == event_count_before + 1 &&
         event_type_is(writer, event_count_before, EventType::MatcherFatal) &&
         writer.events[event_count_before].fatal.reason ==
             FatalReason::NonMonotonicOrderId &&
         writer.events[event_count_before].fatal.offending_order_id == 2 &&
         writer.events[event_count_before].fatal.last_order_id == 2;
}

[[nodiscard]] bool out_of_order_id_is_fatal() {
  ShutdownCommandReader reader;
  CollectingEventWriter writer;
  Matcher matcher(reader, writer, OrderBookConfig{100, 200, 8});

  (void)matcher.process(new_limit(1, 101, Side::Bid, 150, 1));
  (void)matcher.process(new_limit(3, 103, Side::Bid, 151, 1));
  const ProcessResult stale =
      matcher.process(new_limit(2, 102, Side::Bid, 152, 1));

  return stale.status == ProcessStatus::Fatal &&
         stale.fatal_reason == FatalReason::NonMonotonicOrderId &&
         matcher.fatal_reason() == FatalReason::NonMonotonicOrderId &&
         event_type_is(writer, writer.event_count - 1,
                       EventType::MatcherFatal) &&
         writer.events[writer.event_count - 1].fatal.offending_order_id == 2 &&
         writer.events[writer.event_count - 1].fatal.last_order_id == 3;
}

[[nodiscard]] bool first_duplicate_pair_is_fatal() {
  ShutdownCommandReader reader;
  CollectingEventWriter writer;
  Matcher matcher(reader, writer, OrderBookConfig{100, 200, 8});

  const ProcessResult first =
      matcher.process(new_limit(5, 105, Side::Bid, 150, 1));
  const ProcessResult repeated =
      matcher.process(new_limit(5, 205, Side::Bid, 151, 1));

  return first.status == ProcessStatus::Continue &&
         repeated.status == ProcessStatus::Fatal &&
         repeated.fatal_reason == FatalReason::NonMonotonicOrderId &&
         matcher.fatal() &&
         event_type_is(writer, writer.event_count - 1,
                       EventType::MatcherFatal) &&
         writer.events[writer.event_count - 1].fatal.offending_order_id == 5 &&
         writer.events[writer.event_count - 1].fatal.last_order_id == 5;
}

[[nodiscard]] bool rejected_order_id_is_consumed() {
  ShutdownCommandReader reader;
  CollectingEventWriter writer;
  Matcher matcher(reader, writer, OrderBookConfig{100, 200, 8});

  const ProcessResult rejected =
      matcher.process(new_limit(10, 110, Side::Bid, 150, 0));
  const ProcessResult accepted =
      matcher.process(new_limit(11, 111, Side::Bid, 150, 1));

  return rejected.status == ProcessStatus::Continue &&
         accepted.status == ProcessStatus::Continue && !matcher.fatal() &&
         writer.event_count == 3 &&
         event_type_is(writer, 0, EventType::OrderRejected) &&
         event_type_is(writer, 1, EventType::OrderAccepted) &&
         event_type_is(writer, 2, EventType::OrderRested);
}

[[nodiscard]] bool rejected_order_id_cannot_repeat() {
  ShutdownCommandReader reader;
  CollectingEventWriter writer;
  Matcher matcher(reader, writer, OrderBookConfig{100, 200, 8});

  const ProcessResult rejected =
      matcher.process(new_limit(10, 110, Side::Bid, 150, 0));
  const std::size_t event_count_before = writer.event_count;
  const ProcessResult repeated =
      matcher.process(new_limit(10, 210, Side::Bid, 150, 0));

  return rejected.status == ProcessStatus::Continue &&
         repeated.status == ProcessStatus::Fatal &&
         repeated.fatal_reason == FatalReason::NonMonotonicOrderId &&
         writer.event_count == event_count_before + 1 &&
         event_type_is(writer, event_count_before, EventType::MatcherFatal) &&
         writer.events[event_count_before].fatal.reason ==
             FatalReason::NonMonotonicOrderId;
}

} // namespace

int main() {
  if (!rests_non_crossing_limit()) {
    return EXIT_FAILURE;
  }
  if (!matches_fifo_at_resting_price()) {
    return EXIT_FAILURE;
  }
  if (!rejects_zero_quantity()) {
    return EXIT_FAILURE;
  }
  if (!fatal_publish_stops_matcher()) {
    return EXIT_FAILURE;
  }
  if (!accepts_strictly_increasing_order_ids()) {
    return EXIT_FAILURE;
  }
  if (!repeated_order_id_is_fatal()) {
    return EXIT_FAILURE;
  }
  if (!out_of_order_id_is_fatal()) {
    return EXIT_FAILURE;
  }
  if (!first_duplicate_pair_is_fatal()) {
    return EXIT_FAILURE;
  }
  if (!rejected_order_id_is_consumed()) {
    return EXIT_FAILURE;
  }
  if (!rejected_order_id_cannot_repeat()) {
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
