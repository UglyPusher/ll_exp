/**
 * @file types.hpp
 * @brief Public command, event, and status types for fexma::matcher.
 */
#pragma once

#include <cstdint>

#include <fexma/order_book/types.hpp>

namespace fexma::matcher {

using order_book::OrderBookConfig;
using order_book::OrderId;
using order_book::OwnerId;
using order_book::PriceTick;
using order_book::Quantity;
using order_book::Side;

enum class CommandType : std::uint8_t {
  NewLimit,
  Shutdown
};

struct NewLimitOrder {
  OrderId id{};
  OwnerId owner_id{};
  Side side{};
  PriceTick price{};
  Quantity quantity{};
};

struct Command {
  CommandType type{CommandType::Shutdown};
  NewLimitOrder new_limit{};
};

enum class CommandReadStatus : std::uint8_t {
  Ok,
  Empty,
  Fatal
};

struct CommandReadResult {
  CommandReadStatus status{CommandReadStatus::Empty};
  Command command{};

  [[nodiscard]] bool ok() const noexcept {
    return status == CommandReadStatus::Ok;
  }
};

enum class PublishStatus : std::uint8_t {
  Ok,
  Fatal
};

/**
 * @brief Result of publishing one matcher event.
 *
 * Fatal means the writer can no longer provide its publication contract. The
 * event may be definitely not accepted or its acceptance may be unknown; after
 * Fatal the matcher instance is terminal and recovery is external.
 */
struct PublishResult {
  PublishStatus status{PublishStatus::Ok};

  [[nodiscard]] bool ok() const noexcept {
    return status == PublishStatus::Ok;
  }
};

enum class RunStatus : std::uint8_t {
  Stopped,
  Fatal
};

enum class FatalReason : std::uint8_t {
  None,
  CommandReaderFatal,
  EventWriterFatal,
  NonMonotonicOrderId,
  BookInsertFailedAfterExecution
};

struct RunResult {
  RunStatus status{RunStatus::Stopped};
  FatalReason fatal_reason{FatalReason::None};

  [[nodiscard]] bool ok() const noexcept {
    return status == RunStatus::Stopped;
  }
};

enum class ProcessStatus : std::uint8_t {
  Continue,
  Stop,
  Fatal
};

struct ProcessResult {
  ProcessStatus status{ProcessStatus::Continue};
  FatalReason fatal_reason{FatalReason::None};

  [[nodiscard]] bool fatal() const noexcept {
    return status == ProcessStatus::Fatal;
  }
};

enum class EventType : std::uint8_t {
  None,
  OrderAccepted,
  OrderRejected,
  Trade,
  OrderRested,
  OrderDone,
  MatcherFatal
};

enum class RejectReason : std::uint8_t {
  None,
  InvalidQuantity,
  BookInsertFailed,
  UnknownCommand
};

struct TradeEvent {
  OrderId taker_order_id{};
  OrderId maker_order_id{};
  OwnerId taker_owner_id{};
  OwnerId maker_owner_id{};
  PriceTick price{};
  Quantity quantity{};
};

struct OrderRestedEvent {
  OrderId id{};
  OwnerId owner_id{};
  Side side{};
  PriceTick price{};
  Quantity remaining{};
};

struct OrderDoneEvent {
  OrderId id{};
};

struct OrderAcceptedEvent {
  OrderId id{};
};

struct OrderRejectedEvent {
  OrderId id{};
  RejectReason reason{RejectReason::None};
};

struct MatcherFatalEvent {
  FatalReason reason{FatalReason::None};
  OrderId offending_order_id{};
  OrderId last_order_id{};
};

struct Event {
  EventType type{EventType::None};
  OrderAcceptedEvent accepted{};
  TradeEvent trade{};
  OrderRestedEvent rested{};
  OrderDoneEvent done{};
  OrderRejectedEvent rejected{};
  MatcherFatalEvent fatal{};
};

} // namespace fexma::matcher
