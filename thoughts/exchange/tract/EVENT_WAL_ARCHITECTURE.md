# Event WAL Architecture

**Status:** Accepted baseline with explicit open decisions  
**Date:** 2026-08-16

## Purpose

Event WAL is the authoritative ordered history of outcomes produced by the
Matcher thread. It separates deterministic command execution from downstream
delivery and projections.

The baseline tract is:

```text
Matcher -> Event Ring -> Persistence -> durable consumers
```

Matcher never sends a final client response or updates an external projection
directly. It publishes events. Downstream state and responses are derived from
durable events.

## Decision

Event transport is a bounded ring with one producer, one persistence stage, and
multiple downstream consumers.

One Event Ring and its Event WAL file serve exactly one instrument in exactly
one epoch. `(InstrumentId, EpochId)` is fixed by tract configuration and the
manifest and is not repeated in each `EventWalPayload`. A new epoch creates a
new Event WAL file.

The ring has two different parts:

1. Matcher to Persistence is a sequential publication stage.
2. After the durable frontier, required consumers form a broadcast SPMC tract.

```text
                         +-> MarketDataProjection
Matcher -> Persistence -+-> Reserve reconciliation
                         +-> Client Egress
                         +-> other required projections
```

All consumers observe the same immutable event sequence. They advance
independently and never modify event payloads.

## Roles

### Matcher thread

Matcher is the sole producer of events. It:

- transforms one ordered command into one or more ordered events;
- assigns deterministic event sequences;
- records causation to the source command;
- retains at most the final event of the current command and publishes events
  with an explicit command-result boundary;
- forgets the published data after successful publication.

The Matcher thread also publishes command rejections produced by earlier
pipeline stages. The matching core itself receives only accepted commands.

### Persistence

Persistence is the sole writer of the durable frontier. It:

- reads published events in order;
- serializes physical Event WAL records;
- appends a selected batch;
- performs one physical durability operation for the batch;
- advances `durable` only after the complete operation succeeds.

Persistence does not assign business meaning, reinterpret events, or update
projections.

### Durable consumers

Every required consumer:

- reads only events below `durable`;
- processes events strictly in event-sequence order;
- applies events and advances progress only for a complete command result ending
  with `is_last_for_command`;
- owns and publishes its own progress cursor;
- is independently rebuildable from Event WAL;
- never modifies the shared event block.

Known durable consumers include:

- `MarketDataProjection`;
- reserve consumption/release reconciliation;
- `ClientEgress` response generation;
- future accounting and client projections.

Metrics, tracing, and other lossy observers are not required consumers and must
not retain Event Ring slots.

## Positions and invariants

All positions are absolute, monotonically increasing, and exclusive
boundaries.

```text
consumer_progress[i] <= durable <= head
head - reclaim <= capacity
```

| Position | Sole writer | Meaning |
|---|---|---|
| `head` | Matcher thread | One past the last event published to the ring |
| `durable` | Persistence | One past the last event confirmed physically durable |
| `consumer_progress[i]` | Consumer `i` | One past the last event fully applied by that consumer |
| `reclaim` | Derived | Minimum progress of all required consumers |

```text
reclaim = min(consumer_progress[required consumers])
```

If there are no registered downstream consumers during initialization,
`durable` is the reclamation boundary. The baseline runtime normally has at
least one required consumer.

Matcher may reuse a slot only when `reclaim` has passed its previous absolute
position.

## Event identity and causation

The in-memory envelope and logical Event WAL payload are:

```cpp
struct EventEnvelope {
    EventSequence event_sequence;
    EventWalPayload payload;
};

struct EventWalPayload {
    ClientId client_id;
    CommandSequence caused_by_command_sequence;
    EventIndex index_in_command;
    bool is_last_for_command;
    Event message;
};
```

- `event_sequence` equals the physical Event WAL `RecordHeader::sequence`,
  starts at `1`, is local to that instrument and epoch, and is not repeated in
  `EventWalPayload`.
- `client_id` preserves the accepted command's client for downstream consumers.
- `caused_by_command_sequence` identifies the source command.
- `index_in_command` gives deterministic order among events produced by one
  command.
- indices for one command start at `0` and are contiguous; exactly the final
  event has `is_last_for_command == true`.
- Event sequence is assigned by Matcher, not Persistence.
- Instrument, epoch, and event schema version belong to Event WAL file/manifest
  metadata and are not repeated in each payload.

Persistence preserves the order already established by Matcher.

## Event payload

Event payload is immutable after Matcher publishes `head`. It remains valid
until all required consumers advance past it and the slot is reclaimed.

Representative event classes include:

- `RiskRejected`;
- `ReserveRejected`;
- `OrderRested`;
- `OrderPartiallyFilled`;
- `OrderFullyFilled`;
- `TradeExecuted`;
- `OrderCancelled`;
- session and incident events.

Rejection events contain sufficient provenance for audit:

```text
source command sequence
rejecting module
reason and rule identifier
epoch
module version
ruleset identifier
```

The epoch manifest remains the authoritative source for complete build and
configuration provenance. Rejection events repeat the directly relevant risk
or reserve provenance intentionally.

## Publication and memory ordering

The publication chain is:

```text
event payload
  -> head
  -> physical append and sync
  -> durable
  -> consumer processing
  -> consumer_progress[i]
  -> reclaim
  -> slot reuse
```

- Matcher completes event writes before release-publishing `head`.
- Persistence acquire-loads `head` before reading event data.
- Persistence completes physical durability before release-publishing
  `durable`.
- Consumers acquire-load `durable` before reading event data.
- A consumer completes its work before release-publishing its progress.
- Matcher acquires the reclamation boundary before reusing slots.

`head`, `durable`, and independently written consumer cursors must not share a
cache line. Each consumer progress position resides in its own padded area.

## Reclamation and consumer classes

Required consumers participate in reclamation and therefore in back-pressure.
Losing or stalling a required consumer eventually stops Matcher by filling the
ring.

Optional consumers do not participate in `reclaim`. They must tolerate loss,
consume from a copied transport, or resynchronize from Event WAL/snapshot.

A consumer cannot silently change between required and optional during an
epoch. The required-consumer set is fixed during initialization and recorded in
the epoch/runtime configuration.

Dynamic consumer registration and removal are outside the baseline design.

## Back-pressure and failure

The Event Ring is full when:

```text
head - reclaim == capacity
```

Failure propagation is local and mechanical:

- If Persistence stalls, `durable` stops. Consumers drain only the already
  durable range. Matcher eventually reaches `Full`.
- If a required consumer stalls, its progress becomes `reclaim`. Matcher
  eventually reaches `Full`, even if all other consumers are current.
- If an optional observer stalls, it does not affect reclamation.
- If physical append or sync fails, `durable` remains unchanged. No consumer
  may observe the failed range as durable.

No central resource orchestrator is required. Health propagates upstream
through bounded capacity. The established yellow/red/purple health state may
be derived from ring occupancy and downstream state.

## Client responses

Ingress receipt and final business outcome are different facts.

Final client responses are generated from durable Event WAL events, not from a
direct Matcher callback. This ensures that a response does not claim an outcome
that has not crossed the durable frontier.

Typical outcomes are:

```text
RiskRejected
ReserveRejected
OrderAccepted / OrderRested
TradeExecuted
OrderCancelled
```

Network retry, delivery acknowledgement, per-client ordering, and response
deduplication belong to Client Egress and its projection, not to Matcher or
Event WAL persistence.

## Reserve feedback

ReserveManager creates or rejects a reservation before Matcher in the Command
SPMC Pipeline Ring. Matcher events then describe how the reservation is used or
released:

- execution consumes the corresponding reserved amount;
- cancellation releases the remaining reservation;
- terminal rejection releases any reservation that was already created;
- partial execution consumes part and retains the required remainder.

Reserve reconciliation consumes durable events. Its exact ordering relative to
new command-side reserve decisions is a separate design decision because this
introduces feedback from Event WAL into the stateful ReserveManager.

## Replay and recovery

Event WAL is the primary source for rebuilding downstream state:

```text
snapshot + durable Event WAL suffix -> reconstructed projection
```

Event replay:

- applies recorded events in event-sequence order;
- applies a command result only after validating contiguous indices and
  `is_last_for_command == true`;
- discards an incomplete command result at the end of the WAL;
- does not rerun PreRisk, ReserveManager decisions, or Matcher;
- verifies physical record integrity and sequence continuity;
- stops on schema mismatch, corruption, or an unexplained sequence gap.

Command replay is a different operation. It reruns the deterministic command
tract using the original epoch build, rules, configuration, and initial state,
then compares the produced Event WAL with the recorded one.

## Epochs and versions

One Event WAL belongs to one instrument and one epoch; `InstrumentId` and
`EpochId` are not repeated in every `EventWalPayload`. The epoch manifest fixes
all inputs that can change the deterministic Command WAL to Event WAL
transformation:

- engine build;
- Matcher version;
- PreRisk and ReserveManager versions;
- rulesets and limits;
- command and event schemas;
- instrument configuration;
- starting snapshot and other reference data.

Module, rule, schema, or deterministic configuration changes begin a new
epoch. The baseline procedure drains and durably closes the old command and
event tracts before the new epoch starts.

## Event batches

One command may produce multiple ordered events. They form one logical command
result:

```text
Command N -> Event N.0, Event N.1, ... Event N.k
```

Matcher retains one final event for the current command. When another event is
produced, the previous one is published with
`is_last_for_command == false`; command completion publishes the retained event
with `is_last_for_command == true`. There is no separate `CommandCompleted`
event.

Consumers may read or make durable a prefix of a multi-event result, but must
not apply it as a completed business result before the final event. After a
crash, recovery validates equal `ClientId` and
`caused_by_command_sequence`, contiguous indices starting at `0`, and the final
flag. It truncates the tail after the last fully validated command result.

Every accepted command must produce at least one event. Therefore
`ShutdownCommand` publishes `ShutdownEvent`. `CommandReaderFatal` has no
accepted command and is reported out-of-band instead of using fabricated zero
identities.

## Open decisions

1. **Required consumers.** Finalize which projections participate in
   reclamation. In particular, decide whether Client Egress is required or may
   rebuild/resume independently from Event WAL.
2. **Reserve feedback ordering.** Define one deterministic order between new
   command-side reservations and durable trade/cancel/rejection feedback.
3. **Snapshot consumer.** Decide whether snapshotting is a required ring
   consumer or operates from an independently maintained projection.
4. **Consumer checkpoints.** Define which progress positions must themselves be
   durable and how they are restored without skipping events.

## Consequences

- Matcher remains single-writer and deterministic.
- No downstream projection can observe a non-durable outcome.
- Required projections receive every event in the same total order.
- Slow required consumers produce bounded back-pressure rather than data loss.
- Optional telemetry cannot stop the engine.
- Event replay rebuilds projections without requiring old decision modules.
- Reclamation is more general than in the Command SPMC Pipeline Ring: Event WAL
  requires the minimum progress of independent required consumers.

## Out of scope

- detailed physical Event WAL record layout;
- Client Egress transport protocol;
- snapshot file format;
- accounting implementation;
- cross-NUMA consumer aggregation;
- dynamic consumers;
- replication and remote Event WAL followers;
- exact ReserveManager reconciliation algorithm.
