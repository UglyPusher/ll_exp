# SPMC Pipeline Ring

**Status:** Accepted architectural decision  
**Date:** 2026-08-16

## Decision

The command tract from Ingress to Matcher is implemented as one bounded
**SPMC pipeline ring with sequential readiness frontiers**.

One ring and its Command WAL file serve exactly one instrument in exactly one
epoch. `(InstrumentId, EpochId)` is fixed by tract configuration and the
manifest and is not repeated in each `CommandWalPayload`. A new epoch creates a
new Command WAL file. `CommandSequence` is monotonic within that file.

`CommandSequence` equals the physical Command WAL
`RecordHeader::sequence`. There is no separate counter or mapping table.
Ingress derives the sequence from the next absolute ring position before
publishing the command, and Persistence writes the same value into the physical
record header.

One accepted incoming command corresponds to exactly one physical Command WAL
record. This applies equally to trading, control, and financial commands: a
record never contains a command batch, and one command never spans records.
Command WAL ownership for financial commands remains an explicit open decision
because those commands are not naturally instrument-scoped. The decision must
establish deterministic ordering between a portfolio withdrawal and concurrent
reserve creation or trade execution; independent WAL-local sequences are not
sufficient by themselves.

Every record in one Command WAL has the same canonical `payload_size`, large
enough for the largest command in the current schema version. A shorter command
zero-fills the unused payload tail. `RecordHeader` has no variable payload
length, and canonical size is not derived from native
`sizeof(CommandWalPayload)`.

Every `CommandWalPayload` contains the `ClientId` recorded by Ingress together
with the command. There is no `ClientSequence` in the payload: its invariant,
validation, reconnect behavior, and recovery role remain undefined.

Ingress is the only producer of command blocks. Persistence, PreRisk,
ReserveManager, and Matcher are consumers of the same immutable command. Unlike
a broadcast SPMC queue, consumers are ordered: each stage can process only the
range published by the preceding stage and publishes its own frontier for the
next stage.

```text
Ingress -> Persistence -> PreRisk -> ReserveManager -> Matcher
```

## Frontiers

All frontiers are absolute monotonically increasing positions and exclusive
boundaries:

```text
tail <= reserve_checked <= risk_checked <= durable <= head
head - tail <= capacity
```

| Frontier | Sole writer | Meaning |
|---|---|---|
| `head` | Ingress | One past the last command published into the ring |
| `durable` | Persistence | One past the last physically durable command |
| `risk_checked` | PreRisk | One past the last command with a published risk decision |
| `reserve_checked` | ReserveManager | One past the last command with a published reserve decision |
| `tail` | Matcher thread | One past the last finally consumed command; earlier slots may be reused |

Each stage processes its own range:

```text
Persistence:    [durable,         head)
PreRisk:        [risk_checked,    durable)
ReserveManager: [reserve_checked, risk_checked)
Matcher:        [tail,            reserve_checked)
```

The progress plane is a chain of SPSC publications over one shared SPMC data
plane:

```text
Ingress        -- head -----------> Persistence
Persistence    -- durable --------> PreRisk
PreRisk        -- risk_checked ---> ReserveManager
ReserveManager -- reserve_checked -> Matcher
Matcher        -- tail -----------> Ingress
```

## Command block

Ingress writes the command before publishing `head`. The command is immutable
until Matcher advances `tail` and the slot becomes reusable.

Each checking module owns a separate decision area in the block. A module may
write only its own result.

```cpp
struct CommandRingSlot {
    CommandEnvelope command;  // Ingress; sequence + immutable WAL payload
    RiskResult risk;          // PreRisk; runtime-only sidecar
    ReserveResult reserve;    // ReserveManager; runtime-only sidecar
};

enum class Decision : std::uint8_t {
    Pending,
    Accepted,
    Rejected,
    Skipped
};
```

Persistence writes `CommandEnvelope::command_sequence` only to
`RecordHeader::sequence` and serializes `CommandEnvelope::payload` as
`CommandWalPayload`. The sequence is not duplicated in the payload.

`RiskResult` and `ReserveResult` are in-memory sidecar metadata. They are not
part of the immutable `CommandWalPayload`, are not serialized into Command WAL,
and are not covered by the command-record payload CRC. Checking stages never
mutate an already durable Command WAL record.

The sidecar layout is fixed by the engine build/epoch. No dynamic registry,
map, TLV, or per-command allocation is used.

## Stage contract

For every position below its upstream frontier, a checking stage:

1. reads the immutable command and preceding decisions;
2. writes its own decision exactly once;
3. publishes its advanced frontier with release semantics.

The downstream stage acquires that frontier before reading the decision.

```cpp
while (my_frontier < upstream_frontier) {
    auto& block = ring[my_frontier % capacity];
    block.my_result = check(block);
    publish(++my_frontier);
}
```

If an upstream stage rejected a command, every later checking stage still
passes the position and writes `Skipped`. A rejected command must never stop a
frontier.

## Rejections and Matcher

Matcher thread consumes every fully checked position in order.

- If any decision is `Rejected`, it publishes the corresponding rejection to
  the Event tract and does not pass the command to the matching core. In this
  document, publishing to Event WAL means that the event has been accepted by
  the Event tract; physical Event WAL durability is handled by the Event tract.
- If all required decisions are `Accepted`, it passes the command to the
  matching core.
- Matcher does not produce new terminal command rejections. If a command is
  rejected, the rejecting checking module is the source of the rejection event.
- After the command outcome has been accepted by the Event tract, Matcher
  advances `tail`.

A rejection event identifies at least:

- source command sequence;
- rejecting module;
- reason/rule identifier;
- epoch;
- module and ruleset provenance required for audit.

## Ownership and reclamation

Early consumers never pop or release a slot. They only read the command, write
their own sidecar result, and advance their own frontier.

Matcher is the final consumer and the sole writer of `tail`. Therefore it is
also the logical reclaimer. A general `min(all_consumer_progress)` scan is not
required because the ordered-frontier invariant guarantees that `tail` is
always the minimum progress position.

Ingress may reuse a slot only after acquiring an advanced `tail`.

## Memory ordering and cache layout

Publication follows this happens-before chain:

```text
command
  -> head
  -> durable
  -> risk decision
  -> risk_checked
  -> reserve decision
  -> reserve_checked
  -> matcher consumption
  -> tail
  -> slot reuse
```

Each frontier has exactly one writer. Frontiers written by different threads
must reside in separate cache lines. A stage publishes its data before a
release-store to its frontier; the next stage uses an acquire-load before
reading that data.

Sequential check results may share storage because their writers are ordered by
frontiers. If future checking modules operate in parallel, concurrently written
result areas must not share a cache line.

## Capacity and failure propagation

The ring is full when:

```text
head - tail == capacity
```

No central resource orchestrator is required. A stalled stage naturally stops
all downstream frontiers; upstream stages eventually fill the ring, after
which Ingress receives `Full` and applies its degradation/back-pressure policy.

Persistence failure leaves `durable` unchanged. PreRisk, ReserveManager, and
Matcher may finish already published work up to their respective boundaries,
but no command beyond the last durable position can reach Matcher.

## Epochs and replay

Code, rules, configuration, schemas, and initial state that affect the
deterministic transformation from Command WAL to Event WAL are immutable within
an epoch. An epoch manifest identifies the exact engine build, module versions,
rulesets, schemas, configuration hashes, and starting snapshot.

Commands and events carry `epoch_id`; they do not repeat the full manifest.
Audit-sensitive rejection events additionally carry the rejecting module and
rule provenance.

- Event replay applies already recorded events and does not rerun PreRisk,
  ReserveManager, or Matcher.
- Command replay requires the exact build, rules, configuration, and initial
  state of the original epoch. The baseline assumption is that ReserveManager is
  rerun from the original epoch snapshot during Command replay.
- Changing any deterministic module or ruleset starts a new epoch.

The baseline update procedure drains the old pipeline and durably closes its
Event WAL before opening the new epoch. Supporting in-flight commands from
multiple epochs simultaneously is explicitly out of scope.

## Consequences

- One immutable command block is shared by all pipeline stages.
- There is no per-slot reference count and no independent-reader progress scan.
- Ordering and back-pressure are explicit in the frontier invariant.
- Adding a sequential checking module requires one fixed result area and one
  module-owned frontier.
- ReserveManager is stateful: creation of a reserve is part of the pre-Matcher
  pipeline, while consumption or release of reserves from trades, cancels, and
  rejections requires a separately specified Event WAL feedback contract.

## Out of scope

- NUMA placement and cross-node frontier aggregation;
- parallel independent validators;
- dynamic pipeline composition;
- multiple producers;
- multi-epoch in-flight processing;
- detailed ReserveManager feedback/reconciliation design;
- client response and Event WAL egress tract.
