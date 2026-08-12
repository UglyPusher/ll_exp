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

class EmptyCommandReader {
public:
  [[nodiscard]] CommandReadResult read_next() noexcept {
    return {CommandReadStatus::Shutdown, {}};
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
  EmptyCommandReader reader;
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
  EmptyCommandReader reader;
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
  EmptyCommandReader reader;
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
  EmptyCommandReader reader;
  CollectingEventWriter writer;
  writer.fail_after = 1;
  Matcher matcher(reader, writer, OrderBookConfig{100, 200, 8});

  const ProcessResult processed =
      matcher.process(new_limit(1, 101, Side::Bid, 150, 10));
  const RunResult rerun = matcher.run();

  return processed.status == ProcessStatus::Fatal && matcher.fatal() &&
         rerun.status == RunStatus::Fatal;
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
  return EXIT_SUCCESS;
}
