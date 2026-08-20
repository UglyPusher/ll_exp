/**
 * @file types.hpp
 * @brief Public command, event, and status types for fexma::matcher.
 */
#pragma once

#include <cstdint>
#include <span>
#include <type_traits>

#include <fexma/order_book/order_book.hpp>

namespace fexma::matcher {

using order_book::OrderBookConfig;
using order_book::OrderId;
using order_book::OwnerId;
using order_book::PriceTick;
using order_book::Quantity;
using order_book::Side;

using SnapshotId = std::uint64_t;
using ClientId = std::uint64_t;
using CommandSequence = std::uint64_t;
using EventSequence = std::uint64_t;
using EventIndex = std::uint32_t;
using EpochId = std::uint64_t;
using CommandSchemaVersion = std::uint32_t;
using EventSchemaVersion = std::uint32_t;

inline constexpr CommandSchemaVersion current_command_schema_version = 1;
inline constexpr EventSchemaVersion current_event_schema_version = 1;

enum class CommandType : std::uint8_t {
  None,
  NewLimit,
  SaveSnapshot,
  LoadSnapshot,
  Shutdown
};

struct NewLimitOrder {
  OrderId id{};
  OwnerId owner_id{};
  Side side{};
  PriceTick price{};
  Quantity quantity{};
};

struct SaveSnapshotCommand {
  SnapshotId snapshot_id{};
  EpochId snapshot_epoch_id{};
};

struct LoadSnapshotCommand {
  SnapshotId snapshot_id{};
  EpochId snapshot_epoch_id{};
};

struct ShutdownCommand {};

struct Command {
  constexpr Command() noexcept : type(CommandType::None), none{} {}

  constexpr explicit Command(const NewLimitOrder& command) noexcept
      : type(CommandType::NewLimit), new_limit(command) {}

  constexpr explicit Command(const SaveSnapshotCommand& command) noexcept
      : type(CommandType::SaveSnapshot), save_snapshot(command) {}

  constexpr explicit Command(const LoadSnapshotCommand& command) noexcept
      : type(CommandType::LoadSnapshot), load_snapshot(command) {}

  constexpr explicit Command(const ShutdownCommand& command) noexcept
      : type(CommandType::Shutdown), shutdown(command) {}

  CommandType type;
  union {
    struct {} none;
    NewLimitOrder new_limit;
    SaveSnapshotCommand save_snapshot;
    LoadSnapshotCommand load_snapshot;
    ShutdownCommand shutdown;
  };
};

static_assert(std::is_trivially_copyable_v<Command>);
static_assert(std::is_standard_layout_v<Command>);

/**
 * @brief Logical value encoded into one fixed-size Command WAL payload.
 *
 * The canonical schema defines a fixed payload width large enough for its
 * largest command and zero-fills unused bytes. That width is not sizeof this
 * native C++ carrier.
 */
struct CommandWalPayload {
  ClientId client_id{};
  Command message{};
};

/**
 * @brief In-memory command identity and immutable WAL payload.
 *
 * command_sequence equals the physical Command WAL RecordHeader::sequence and
 * is not repeated in CommandWalPayload. Client identity is part of the
 * persisted payload. Instrument and epoch identity belong to the WAL
 * configuration/manifest and are not repeated in every command.
 */
struct CommandEnvelope {
  CommandSequence command_sequence{};
  CommandWalPayload payload{};
};

enum class CheckDecision : std::uint8_t {
  Pending,
  Accepted,
  Rejected,
  Skipped
};

struct RiskResult {
  CheckDecision decision{CheckDecision::Pending};
  std::uint32_t reason_code{};
};

struct ReserveResult {
  CheckDecision decision{CheckDecision::Pending};
  std::uint32_t reason_code{};
};

/**
 * @brief In-memory command pipeline slot; only command payload is persisted.
 *
 * Ingress is the sole writer of command. RiskManager and ReserveManager are
 * the sole writers of their respective runtime-only sidecars. Persistence
 * writes command.command_sequence into RecordHeader::sequence and canonically
 * encodes command.payload as the record payload.
 */
struct CommandRingSlot {
  CommandEnvelope command{};
  RiskResult risk{};
  ReserveResult reserve{};
};

static_assert(std::is_trivially_copyable_v<CommandWalPayload>);
static_assert(std::is_standard_layout_v<CommandWalPayload>);
static_assert(std::is_trivially_copyable_v<CommandEnvelope>);
static_assert(std::is_standard_layout_v<CommandEnvelope>);
static_assert(std::is_trivially_copyable_v<CommandRingSlot>);
static_assert(std::is_standard_layout_v<CommandRingSlot>);

struct MatcherSnapshotView {
  SnapshotId snapshot_id{};
  CommandSequence command_sequence{};
  EpochId epoch_id{};
  OrderId last_order_id{};
  OrderBookConfig book_config{};
  const order_book::OrderBook* book{};
};

struct MatcherSnapshotImage {
  SnapshotId snapshot_id{};
  CommandSequence command_sequence{};
  EpochId epoch_id{};
  OrderId last_order_id{};
  OrderBookConfig book_config{};
  std::span<const order_book::OrderView> orders{};
};

enum class SnapshotOperationStatus : std::uint8_t {
  Ok,
  Unavailable,
  Invalid,
  CapacityExceeded,
  Fatal
};

struct SnapshotOperationResult {
  SnapshotOperationStatus status{SnapshotOperationStatus::Ok};

  [[nodiscard]] bool ok() const noexcept {
    return status == SnapshotOperationStatus::Ok;
  }
};

enum class CommandReadStatus : std::uint8_t {
  Ok,
  Empty,
  Fatal
};

struct CommandReadResult {
  CommandReadStatus status{CommandReadStatus::Empty};
  CommandEnvelope envelope{};

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
  CommandProducedNoEvent,
  NonMonotonicOrderId,
  BookEraseFailedAfterExecution,
  BookUpdateFailedAfterExecution,
  BookInsertFailedAfterExecution,
  SnapshotCaptureFailed,
  SnapshotLoadUnavailable,
  SnapshotLoadInvalid
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
  SaveSnapshot,
  LoadSnapshot,
  Shutdown,
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

struct SaveSnapshotEvent {
  SnapshotId snapshot_id{};
  EpochId snapshot_epoch_id{};
};

struct LoadSnapshotEvent {
  SnapshotId snapshot_id{};
  EpochId snapshot_epoch_id{};
};

struct ShutdownEvent {};

struct Event {
  constexpr Event() noexcept : type(EventType::None), none{} {}

  constexpr explicit Event(const OrderAcceptedEvent& event) noexcept
      : type(EventType::OrderAccepted), accepted(event) {}

  constexpr explicit Event(const OrderRejectedEvent& event) noexcept
      : type(EventType::OrderRejected), rejected(event) {}

  constexpr explicit Event(const TradeEvent& event) noexcept
      : type(EventType::Trade), trade(event) {}

  constexpr explicit Event(const OrderRestedEvent& event) noexcept
      : type(EventType::OrderRested), rested(event) {}

  constexpr explicit Event(const OrderDoneEvent& event) noexcept
      : type(EventType::OrderDone), done(event) {}

  constexpr explicit Event(const SaveSnapshotEvent& event) noexcept
      : type(EventType::SaveSnapshot), save_snapshot(event) {}

  constexpr explicit Event(const LoadSnapshotEvent& event) noexcept
      : type(EventType::LoadSnapshot), load_snapshot(event) {}

  constexpr explicit Event(const ShutdownEvent& event) noexcept
      : type(EventType::Shutdown), shutdown(event) {}

  constexpr explicit Event(const MatcherFatalEvent& event) noexcept
      : type(EventType::MatcherFatal), fatal(event) {}

  EventType type;
  union {
    struct {} none;
    OrderAcceptedEvent accepted;
    OrderRejectedEvent rejected;
    TradeEvent trade;
    OrderRestedEvent rested;
    OrderDoneEvent done;
    SaveSnapshotEvent save_snapshot;
    LoadSnapshotEvent load_snapshot;
    ShutdownEvent shutdown;
    MatcherFatalEvent fatal;
  };
};

static_assert(std::is_trivially_copyable_v<Event>);
static_assert(std::is_standard_layout_v<Event>);

/**
 * @brief Logical value encoded into one fixed-size Event WAL payload.
 *
 * Every payload is caused by one accepted command. Events for that command use
 * contiguous zero-based indices, and exactly the final event sets
 * is_last_for_command.
 */
struct EventWalPayload {
  ClientId client_id{};
  CommandSequence caused_by_command_sequence{};
  EventIndex index_in_command{};
  bool is_last_for_command{};
  Event message{};
};

/**
 * @brief In-memory Event WAL identity and immutable payload.
 *
 * event_sequence equals the physical Event WAL RecordHeader::sequence and is
 * not repeated in EventWalPayload. Instrument identity, epoch identity, and
 * schema version belong to Event WAL file/manifest metadata.
 */
struct EventEnvelope {
  EventSequence event_sequence{};
  EventWalPayload payload{};
};

static_assert(std::is_trivially_copyable_v<EventWalPayload>);
static_assert(std::is_standard_layout_v<EventWalPayload>);
static_assert(std::is_trivially_copyable_v<EventEnvelope>);
static_assert(std::is_standard_layout_v<EventEnvelope>);

} // namespace fexma::matcher
