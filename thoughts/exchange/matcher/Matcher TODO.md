# Matcher TODO

This file tracks open design issues for the minimal `matcher` sample.

## Architecture checkpoint: resolve before adding more commands

Do not treat an in-memory ring slot, a logical domain message, and a physical
WAL record as the same structure. There are four separate contracts:

1. **Domain schema:** fields and meaning of `Command` and `Event`.
2. **Ring slot:** domain payload plus runtime-only stage sidecars and publication
   metadata.
3. **Physical WAL envelope:** sequence, payload length/format version, CRC, and
   alignment. The generic WAL owns this layer.
4. **Canonical WAL payload encoding:** stable byte encoding of one command or
   event. Native C++ object layout is not a disk format.

The matcher `Command` and tagged-union `Event` define layer 1.
`CommandRingSlot`, `CommandEnvelope`, and `EventEnvelope` define the current
in-memory layer-2 contracts. The generic WAL defines layer 3 for a fixed-size
opaque payload. Canonical layer-4 serializers are not yet connected to matcher
types.

### Accepted: risk decisions and Command WAL

The accepted contract resolves an inconsistency between two older tract
descriptions:

- `TRACT_CONCEPT 0.md` says the risk decision is stored with the command and is
  not recomputed during replay.
- `SPMC_PIPELINE_RING_RU.md` places Persistence before PreRisk and
  ReserveManager, and says their `CheckResult` fields are runtime-only sidecars
  outside the immutable Command WAL payload and its CRC.

An already durable Command WAL record must not be mutated by RiskManager. A
reserved writable area inside that record would make durability and CRC
semantics ambiguous.

Accepted decision:

- [x] **Command WAL contains ingress facts only.** Risk/reserve write only their
  ring sidecars. Normal recovery uses state snapshots plus Event WAL; an audit
  command replay reruns the original deterministic modules/configuration and
  compares the resulting Event WAL.
- [x] The in-memory slot has three separately owned regions:
  `CommandEnvelope` written by Ingress, `RiskResult` written by RiskManager,
  and `ReserveResult` written by ReserveManager.
- [x] Persistence writes `CommandEnvelope::command_sequence` only as physical
  `RecordHeader::sequence` and serializes only `CommandEnvelope::payload` as
  `CommandWalPayload`. Sidecars are neither serialized nor covered by the
  physical command-record payload CRC.
- [x] RiskManager and ReserveManager never mutate an already durable Command
  WAL record. Persisting their decisions in the future would require a separate
  immutable record/log and is not part of the accepted baseline.

### Decision queue

Resolve these in order; later items depend on earlier ones:

- [x] **1a. Command WAL scope.** One Command WAL belongs to exactly one
  instrument and one epoch. `(InstrumentId, EpochId)` is fixed by WAL
  configuration and the epoch manifest; neither value is repeated in every
  `CommandWalPayload`. This intentionally creates one persistence contour per
  instrument and a new WAL file for every new epoch.
- [x] **1b. Command WAL record cardinality.** One accepted incoming command is
  exactly one physical Command WAL record, regardless of whether it is a
  trading, control, or financial command. A record never contains a command
  batch, and one command never spans records.
- [ ] **1c. Command envelope fields.** Define the remaining envelope fields,
  including any command identity beyond the accepted client identity, command
  type, and normalized command body.
- [x] **1c.0 Client identity.** Every `CommandWalPayload` contains `ClientId`.
  It is an ingress fact persisted together with the command and is not inferred
  from the instrument-bound WAL or from `CommandSequence`.
- [x] **1c.1 Command body representation.** `Command` is a trivially-copyable,
  standard-layout tagged union. Typed payload constructors bind `CommandType`
  to the active union member at compile time. Adding commands grows the union
  only to the largest payload. Default construction produces `CommandType::None`,
  never an accidental `Shutdown`.
- [x] **1c.2 Command schema version.** One Command WAL file uses one
  `CommandSchemaVersion`, stored as file-level `payload_schema_version` and in
  the epoch manifest. It is not repeated in `CommandWalPayload` or physical
  record headers. Generic WAL `format_version` remains a separate physical
  format version. The generic WAL header support is implemented; the epoch
  manifest itself remains future tract/runtime work.
- [x] **1c.3 Fixed command payload width.** Every record in one Command WAL has
  the same canonical `payload_size`, large enough for the largest command in
  its `CommandSchemaVersion`. Shorter commands zero-fill the unused canonical
  payload bytes. `RecordHeader` has no per-record payload length. The canonical
  width is schema-defined and must not be derived from native
  `sizeof(CommandWalPayload)`. Storage minimization is explicitly deferred.
- [ ] **1c.4 ClientSequence.** Its purpose and contract are not established.
  A client-provided sequence is not authoritative for ordering inside the
  exchange and is not currently accepted as a deduplication mechanism. Do not
  add `ClientSequence` to `CommandWalPayload` until a concrete invariant,
  validation owner, reconnect behavior, and recovery use are agreed.
- [ ] **1d. Financial-command WAL ownership.** A deposit or withdrawal is not
  naturally instrument-scoped. Decide whether financial commands belong to a
  separate account/ledger Command WAL or whether the previously accepted
  one-WAL-per-instrument scope must be revised. A financial command must not be
  duplicated into several instrument WALs. This remains deliberately open.
  The chosen design must establish one deterministic order between a withdrawal
  from a portfolio and concurrent reserve creation/trade execution against that
  portfolio. Independent WAL-local sequences are insufficient by themselves.
  Define the authoritative owner of available balance, the exact point at which
  funds become unavailable, and the recovery rule for an operation crossing a
  snapshot or crash boundary. Do not implement financial commands before this
  cross-tract ordering contract is accepted.
- [x] **2. Command ring slot ownership.** `CommandEnvelope` is immutable;
  Persistence stores its sequence in the physical header and its
  `CommandWalPayload` in the record payload. `RiskResult` and `ReserveResult`
  are runtime-only writer-owned sidecars that exist until Matcher advances
  `tail`. Exact fields inside both result payloads remain open.
- [x] **3. Event WAL logical record.** `EventEnvelope` contains the
  WAL-derived `EventSequence` and an immutable `EventWalPayload`. The payload
  contains `ClientId`, `caused_by_command_sequence`, zero-based
  `index_in_command`, `is_last_for_command`, and the tagged-union `Event`.
  Instrument, epoch, and event schema version are Event WAL file/manifest
  metadata and are not repeated in every payload.
- [x] **3a. Event WAL scope.** One Event WAL belongs to exactly one instrument
  and one epoch. `(InstrumentId, EpochId)` is fixed by Event WAL configuration
  and the epoch manifest and is not repeated in `EventWalPayload`. This makes
  the instrument-local `caused_by_command_sequence` unambiguous and creates a
  separate Event WAL persistence contour for every instrument and epoch.
- [ ] **4. Sequence algebra.** Separate and define `CommandSequence`,
  `EventSequence`, physical WAL record sequence, ring position, `OrderId`,
  `SnapshotId`, and `EpochId`. Decide which may be numerically equal by
  invariant and which must remain independent types.
- [x] **5. Multi-event command boundary.** Matcher retains at most one pending
  event for the current command. When another event appears, the previous one
  is published with `is_last_for_command == false`; command completion
  publishes the pending event with `is_last_for_command == true`. Events use
  contiguous zero-based `index_in_command` values. There is no separate
  `CommandCompleted` event. Every accepted command must produce at least one
  event; `ShutdownCommand` therefore produces `ShutdownEvent`. Recovery keeps
  the longest valid prefix ending in a validated last event and truncates an
  incomplete final command result. A command handler that produces no event is
  a terminal `CommandProducedNoEvent` implementation-invariant failure.
- [ ] **6. Snapshot boundary.** A snapshot must identify the exact inclusive or
  exclusive Command WAL and Event WAL boundaries it represents for every
  stateful module.
- [x] **7. Canonical encoding.** Schema version 1 uses explicit little-endian
  fixed-width fields, a 48-byte Command payload, a 64-byte Event payload,
  validated tags/enums, canonical boolean values, and a zeroed unused tail.
  Unknown or non-canonical values are rejected before a domain object is
  returned. Physical WAL supplies the payload CRC. Exact offsets and schema
  evolution rules are frozen in `exchange/matcher/WAL_PAYLOAD_FORMAT.md`.

### Sequence facts already implied by the design

- Generic WAL physical sequences are local to one WAL file, start at `1`, and
  are contiguous in append order.
- Command and Event WAL physical sequences belong to different sequence
  domains even if both start at `1`.
- [x] `CommandSequence` is exactly the physical
  `wal::RecordHeader::sequence` of its Command WAL record. There is no second
  command-sequence counter or mapping table.
- `CommandSequence` starts at `1`, is contiguous, and is scoped to one
  instrument-bound Command WAL within an epoch. Ingress derives it from the
  next Command WAL/ring position before publishing the command; Persistence
  writes the same value into the physical record header.
- [x] Canonical `CommandWalPayload` bytes do not repeat `CommandSequence`.
  Recovery reconstructs `CommandEnvelope::command_sequence` exclusively from
  the validated physical `RecordHeader::sequence`.
- [x] Canonical `CommandWalPayload` bytes do not contain `EpochId`. One Command
  WAL file belongs to one epoch, and recovery obtains the epoch from validated
  WAL configuration/manifest metadata.
- [x] Canonical `CommandWalPayload` bytes contain `ClientId`; client identity
  is preserved by Command WAL replay.
- A domain `EventSequence` is assigned by Matcher, not Event Persistence.
- [x] `EventSequence` is exactly the physical
  `wal::RecordHeader::sequence` of its Event WAL record and is not repeated in
  `EventWalPayload`. It is scoped to one instrument-and-epoch Event WAL. A
  fresh Event WAL starts at `1`; recovery supplies Matcher with the next
  sequence after the validated Event WAL prefix.
- Every Event WAL entry is caused by one accepted command and carries its
  `ClientId` and `caused_by_command_sequence`. Events from one command are
  contiguous and carry deterministic zero-based indices.
- `OrderId` is business identity, not a WAL position or command sequence.
- [x] Snapshot command payloads do not carry `command_sequence`. Matcher accepts
  `CommandEnvelope`, uses its WAL-derived sequence as the saved snapshot
  boundary, and publishes it as `caused_by_command_sequence` in the common
  `EventWalPayload` around either barrier event. A loaded snapshot image carries
  its own saved command boundary.
- [x] `SaveSnapshotCommand` and `LoadSnapshotCommand` carry
  `snapshot_epoch_id`. It identifies the target snapshot epoch and is not the
  epoch of the enclosing Command WAL record. Snapshot barrier events preserve
  the same target identity.

## CommandReader

- Graceful shutdown is now only an ordered `CommandType::Shutdown` command.
- `ShutdownCommand` publishes a final `ShutdownEvent`, so downstream stateful
  modules observe shutdown in the same ordered Event WAL tract.
- `CommandReadStatus` is limited to `Ok`, `Empty`, and `Fatal`.
- `Empty` means polling: the matcher remains in `run()`, performs no state
  transition, and reads again. The current sample does no pause instruction,
  sleep, yield, idle callback, or batch drain.
- Define the concrete production idle strategy: pure spin, pause/backoff,
  metrics hook, or reader-owned wait policy.
- Decide whether `read_batch()` is useful. It may help amortize reader overhead
  or drain a ring/WAL page, but it is intentionally not part of the first
  contract.
- `CommandReadStatus::Fatal` makes the matcher terminal and returns
  `RunStatus::Fatal`. It has no accepted command, `ClientId`, or
  `CommandSequence`, so it is reported only through runtime/executor channels
  and is never fabricated as an Event WAL entry with zero identities.

## EventWriter

- `publish()` has only `Ok` and `Fatal`. Temporary capacity pressure is hidden
  inside the writer and may block until space is available.
- Define the exact meaning of `Ok` for each writer: accepted into an in-memory
  queue, written into mmap, made durable, or made visible to a downstream
  consumer.
- `Fatal` means the writer can no longer provide its publication contract. The
  failing event may be definitely not accepted or may have unknown publication
  status; recovery code must tolerate a valid prefix plus a possibly ambiguous
  tail.
- Confirm that a blocking `publish()` is acceptable in the synchronous matcher
  path.
- After `Fatal`, the matcher is terminal. Recovery is external via the last
  valid snapshot plus replay of the valid ordered command stream.
- Snapshot save durability failures are outside the matcher `EventWriter`
  contract. The matcher only synchronously validates local capture/load.
- The high-level diagnostics routing policy is defined: a command-caused fatal
  may produce a final `MatcherFatal`, while `CommandReaderFatal` and
  `EventWriterFatal` are reported through `RunResult` and out-of-band
  runtime/executor channels. The exact diagnostic payload, logging, metrics,
  and runtime reporting format remain open.

## Order identity and preflight

- `OrderId` is assigned upstream. Within one matcher epoch, new-order IDs must
  be strictly monotonically increasing: `order_id > last_order_id_`.
- `(EpochId, OrderId)` identifies an order globally. Epoch infrastructure is
  not part of the current sample.
- A stale, duplicate, or out-of-order new-order ID is a fatal stream invariant
  violation, not a business rejection.
- Stream/system invariant fatal reasons such as `NonMonotonicOrderId` publish a
  terminal `MatcherFatal` event while the event writer is still healthy.
- `EventWriterFatal` cannot publish a reliable fatal event through the failed
  writer; runtime/executor diagnostics remain out-of-band for that case.
- The matcher does not perform an active-book duplicate lookup for new orders;
  monotonicity is the hot-path duplicate/stale guard.
- In the current sample, if an order partially executes and then cannot rest its
  remaining quantity, the matcher treats that as fatal because trade events have
  already been published and there is no rollback.
- Business rejection does not roll back `last_order_id_`; the ID is consumed
  after the monotonicity check passes.

## Matching semantics

- Only limit orders are modeled. Market orders, IOC/FOK, post-only, reduce-only,
  pegged orders, and auction orders are not defined.
- Trade price is the resting maker price. This should be stated as a formal
  matching rule.
- Self-trade prevention is not implemented. Decide whether it belongs inside
  matcher policy or in an upstream validation/risk layer.
- Account state, risk checks, balances, positions, and credit limits are out of
  scope for this sample.
- Price range behavior for aggressive orders outside the book's configured
  range is not fully specified.

## Event stream

- Event ordering needs a formal contract. The sample emits accepted, then zero
  or more trades/maker done events, then taker done or rested/rejected.
- Partial maker updates currently have no event. Decide whether the event stream
  needs an explicit `OrderReduced`/`OrderUpdated` event.
- Rejection events are minimal. They do not yet expose the underlying
  `InsertStatus` from `OrderBook`.
- Command sequence numbers are not yet carried through ordinary matcher
  processing. Instrument identity is intentionally stream-level rather than a
  per-command field. Timestamps and partition IDs are not modeled.

## Snapshot barriers

- `SaveSnapshot` and `LoadSnapshot` are ordered barrier commands shared by all
  stateful tract modules.
- The matcher completes `SaveSnapshot` after copying its local state into
  snapshot-writer memory and publishing the corresponding barrier event to
  Event WAL. Disk persistence is asynchronous and handled by recovery/runtime
  infrastructure.
- The matcher completes `LoadSnapshot` after replacing its local state from an
  image supplied by the snapshot store and publishing the corresponding barrier
  event to Event WAL. The hot-path matcher contract does not include disk reads.
- The current sample stores only matcher-local identity and book state. The
  production snapshot image still needs schema version, command/event boundary,
  config hash, instrument/partition identity, and checksum policy.
- Decide whether downstream stateful modules need separate ack/failure events
  or whether runtime diagnostics are enough for their local barrier failures.

## Lifecycle and FSM

- `Matcher::run()` is synchronous and blocking. It executes in the
  caller-prepared thread; matcher creates, configures, pins, and stops no
  threads. Executor/main/runtime owns that execution infrastructure.
- The sample has no real market FSM beyond running/stopped/fatal.
- Auction, halt, resume, warmup, and controlled drain are open.
- `process()` is public for tests and simple harnesses. Decide whether the
  production API should expose it or keep it behind a test adapter.
- After fatal, all calls except diagnostics/destruction should be considered
  invalid. The sample returns fatal defensively.

## Allocation and performance

- The matcher itself is allocation-free after `OrderBook` construction. The
  sample test writer uses a fixed array and reports fatal on overflow.
- Compile-time reader/writer composition avoids virtual dispatch. If runtime
  polymorphism is later needed, measure the cost before changing the hot path.
- No cache-line layout, prefetching, batching, pause strategy, or NUMA-specific
  construction policy is defined yet.
