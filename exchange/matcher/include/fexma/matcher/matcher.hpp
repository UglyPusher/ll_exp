/**
 * @file matcher.hpp
 * @brief Minimal single-writer matcher sample built on OrderBook.
 */
#pragma once

#include <algorithm>
#include <optional>

#include <fexma/matcher/types.hpp>
#include <fexma/order_book/order_book.hpp>

namespace fexma::matcher {

template <class CommandReader, class EventWriter>
class Matcher final {
public:
  Matcher(CommandReader& commands, EventWriter& events,
          const OrderBookConfig& book_config)
      : commands_(commands), events_(events), book_(book_config) {}

  Matcher(const Matcher&) = delete;
  Matcher& operator=(const Matcher&) = delete;
  Matcher(Matcher&&) = delete;
  Matcher& operator=(Matcher&&) = delete;

  [[nodiscard]] RunResult run() noexcept {
    if (fatal_) {
      return {RunStatus::Fatal, fatal_reason_};
    }

    while (running_) {
      const CommandReadResult read = commands_.read_next();
      switch (read.status) {
      case CommandReadStatus::Ok: {
        const ProcessResult processed = process(read.command);
        if (processed.status == ProcessStatus::Stop) {
          return {RunStatus::Stopped};
        }
        if (processed.status == ProcessStatus::Fatal) {
          return {RunStatus::Fatal, processed.fatal_reason};
        }
        break;
      }
      case CommandReadStatus::Empty:
        break;
      case CommandReadStatus::Fatal:
        return enter_fatal(FatalReason::CommandReaderFatal, {}, {});
      }
    }

    return {RunStatus::Stopped};
  }

  [[nodiscard]] ProcessResult process(const Command& command) noexcept {
    if (fatal_) {
      return {ProcessStatus::Fatal, fatal_reason_};
    }

    switch (command.type) {
    case CommandType::NewLimit:
      return process_new_limit(command.new_limit);
    case CommandType::Shutdown:
      running_ = false;
      return {ProcessStatus::Stop};
    }

    if (!publish_rejected({}, RejectReason::UnknownCommand)) {
      return {ProcessStatus::Fatal, fatal_reason_};
    }
    return {ProcessStatus::Continue};
  }

  [[nodiscard]] bool fatal() const noexcept {
    return fatal_;
  }

  [[nodiscard]] FatalReason fatal_reason() const noexcept {
    return fatal_reason_;
  }

  [[nodiscard]] const order_book::OrderBook& book() const noexcept {
    return book_;
  }

private:
  [[nodiscard]] ProcessResult process_new_limit(
      const NewLimitOrder& incoming) noexcept {
    if (incoming.id <= last_order_id_) {
      return enter_fatal_process(FatalReason::NonMonotonicOrderId,
                                 incoming.id, last_order_id_);
    }
    last_order_id_ = incoming.id;

    if (incoming.quantity == 0) {
      if (!publish_rejected(incoming.id, RejectReason::InvalidQuantity)) {
        return {ProcessStatus::Fatal, fatal_reason_};
      }
      return {ProcessStatus::Continue};
    }

    if (!publish_accepted(incoming.id)) {
      return {ProcessStatus::Fatal, fatal_reason_};
    }

    Quantity remaining = incoming.quantity;
    const Side opposite = opposite_side(incoming.side);

    while (remaining != 0) {
      const std::optional<order_book::OrderView> resting =
          book_.best(opposite);
      if (!resting || !crosses(incoming.side, incoming.price, resting->price)) {
        break;
      }

      const Quantity executed =
          (std::min)(remaining, resting->remaining);
      if (!publish_trade(incoming, *resting, executed)) {
        return {ProcessStatus::Fatal, fatal_reason_};
      }

      remaining -= executed;
      const Quantity resting_remaining = resting->remaining - executed;
      if (resting_remaining == 0) {
        const order_book::EraseResult erased = book_.erase(resting->id);
        if (!erased.ok() || !publish_done(resting->id)) {
          return {ProcessStatus::Fatal, fatal_reason_};
        }
      } else {
        const order_book::SetRemainingResult changed =
            book_.set_remaining(resting->id, resting_remaining);
        if (!changed.ok()) {
          return {ProcessStatus::Fatal, fatal_reason_};
        }
      }
    }

    if (remaining == 0) {
      if (!publish_done(incoming.id)) {
        return {ProcessStatus::Fatal, fatal_reason_};
      }
      return {ProcessStatus::Continue};
    }

    const order_book::InsertResult inserted =
        book_.insert({incoming.id, incoming.owner_id, incoming.side,
                      incoming.price, remaining});
    if (!inserted.ok()) {
      if (remaining == incoming.quantity) {
        if (!publish_rejected(incoming.id, RejectReason::BookInsertFailed)) {
          return {ProcessStatus::Fatal, fatal_reason_};
        }
        return {ProcessStatus::Continue};
      }
      return enter_fatal_process(FatalReason::BookInsertFailedAfterExecution,
                                 incoming.id, last_order_id_);
    }

    if (!publish_rested(incoming, remaining)) {
      return {ProcessStatus::Fatal, fatal_reason_};
    }
    return {ProcessStatus::Continue};
  }

  [[nodiscard]] static Side opposite_side(Side side) noexcept {
    return side == Side::Bid ? Side::Ask : Side::Bid;
  }

  [[nodiscard]] static bool crosses(Side incoming_side,
                                    PriceTick incoming_price,
                                    PriceTick resting_price) noexcept {
    return incoming_side == Side::Bid ? incoming_price >= resting_price
                                      : incoming_price <= resting_price;
  }

  [[nodiscard]] bool publish_accepted(OrderId id) noexcept {
    Event event{};
    event.type = EventType::OrderAccepted;
    event.accepted.id = id;
    return publish(event);
  }

  [[nodiscard]] bool publish_rejected(OrderId id,
                                      RejectReason reason) noexcept {
    Event event{};
    event.type = EventType::OrderRejected;
    event.rejected.id = id;
    event.rejected.reason = reason;
    return publish(event);
  }

  [[nodiscard]] bool publish_trade(const NewLimitOrder& taker,
                                   const order_book::OrderView& maker,
                                   Quantity quantity) noexcept {
    Event event{};
    event.type = EventType::Trade;
    event.trade.taker_order_id = taker.id;
    event.trade.maker_order_id = maker.id;
    event.trade.taker_owner_id = taker.owner_id;
    event.trade.maker_owner_id = maker.owner_id;
    event.trade.price = maker.price;
    event.trade.quantity = quantity;
    return publish(event);
  }

  [[nodiscard]] bool publish_rested(const NewLimitOrder& order,
                                    Quantity remaining) noexcept {
    Event event{};
    event.type = EventType::OrderRested;
    event.rested.id = order.id;
    event.rested.owner_id = order.owner_id;
    event.rested.side = order.side;
    event.rested.price = order.price;
    event.rested.remaining = remaining;
    return publish(event);
  }

  [[nodiscard]] bool publish_done(OrderId id) noexcept {
    Event event{};
    event.type = EventType::OrderDone;
    event.done.id = id;
    return publish(event);
  }

  [[nodiscard]] bool publish_matcher_fatal(FatalReason reason,
                                           OrderId offending_order_id,
                                           OrderId last_order_id) noexcept {
    Event event{};
    event.type = EventType::MatcherFatal;
    event.fatal.reason = reason;
    event.fatal.offending_order_id = offending_order_id;
    event.fatal.last_order_id = last_order_id;
    return publish(event);
  }

  [[nodiscard]] bool publish(const Event& event) noexcept {
    const PublishResult published = events_.publish(event);
    if (!published.ok()) {
      fatal_ = true;
      running_ = false;
      fatal_reason_ = FatalReason::EventWriterFatal;
      return false;
    }
    return true;
  }

  [[nodiscard]] RunResult enter_fatal(FatalReason reason,
                                      OrderId offending_order_id,
                                      OrderId last_order_id) noexcept {
    if (reason != FatalReason::EventWriterFatal &&
        !publish_matcher_fatal(reason, offending_order_id, last_order_id)) {
      return {RunStatus::Fatal, fatal_reason_};
    }
    mark_fatal(reason);
    return {RunStatus::Fatal, fatal_reason_};
  }

  [[nodiscard]] ProcessResult enter_fatal_process(
      FatalReason reason, OrderId offending_order_id,
      OrderId last_order_id) noexcept {
    if (reason != FatalReason::EventWriterFatal &&
        !publish_matcher_fatal(reason, offending_order_id, last_order_id)) {
      return {ProcessStatus::Fatal, fatal_reason_};
    }
    mark_fatal(reason);
    return {ProcessStatus::Fatal, fatal_reason_};
  }

  void mark_fatal(FatalReason reason) noexcept {
    fatal_ = true;
    running_ = false;
    fatal_reason_ = reason;
  }

  CommandReader& commands_;
  EventWriter& events_;
  order_book::OrderBook book_;
  OrderId last_order_id_{};
  bool running_{true};
  bool fatal_{false};
  FatalReason fatal_reason_{FatalReason::None};
};

} // namespace fexma::matcher
