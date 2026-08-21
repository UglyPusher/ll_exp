# fexma::matcher

`fexma::matcher` is a minimal matching-component sketch built on top of
`fexma::order_book`.

It owns an `OrderBook`, consumes commands from a caller-provided reader, and
publishes events through a caller-provided writer. The matcher does not create
threads, configure CPU affinity, know about WAL files, or own transport.
`Matcher::run()` is synchronous and blocking; it executes in the caller's
already-prepared thread.

The hot-path contract is intentionally small:

- one matcher instance has one mutable matching state;
- commands are processed strictly sequentially;
- `CommandReader` and public `process()` provide a `CommandEnvelope`; raw
  `Command` payloads are never processed without their WAL-derived sequence;
- every persisted `CommandWalPayload` contains `ClientId`; no client-provided
  sequence participates in matcher ordering;
- `Command` is a tagged union built from typed command payloads; its size grows
  with the largest payload rather than the sum of every supported command;
- default `Command{}` has type `None` and cannot stop the matcher accidentally;
- Command WAL uses one fixed canonical payload width per schema version, sized
  for the largest command; shorter commands zero-fill unused payload bytes;
- `CommandReader::read_next()` may return `Ok`, `Empty`, or `Fatal`;
- graceful shutdown is an ordered `CommandType::Shutdown` command that
  publishes a final `ShutdownEvent` for downstream stateful modules;
- `SaveSnapshot` and `LoadSnapshot` are ordered barrier commands shared by
  stateful tract stages;
- `SaveSnapshot` completes after matcher state has been copied into
  snapshot-writer memory and the corresponding barrier event has been published
  to the event stream; disk persistence is asynchronous and outside the matcher
  fatal path;
- `LoadSnapshot` replaces matcher state from a snapshot image already available
  through the snapshot store and publishes the corresponding barrier event to
  the event stream; hot-path disk reads are outside the matcher;
- new-order `OrderId` values are strictly monotonically increasing within one
  matcher epoch;
- `EventWriter::publish(const EventEnvelope&)` returns only `Ok` or `Fatal`;
- temporary writer capacity pressure is handled inside the writer and is not
  observable by the matcher;
- after `Fatal`, the matcher instance must not continue processing commands.

`Empty` means the matcher remains in its polling loop and performs no state
transition. The concrete pause/backoff strategy is deliberately not modeled in
this sample.

Every accepted command produces at least one event. Events from one command
carry contiguous zero-based indices. Matcher retains one pending event: it
publishes the previous event as non-final when the next appears, and publishes
the final pending event with `is_last_for_command == true`. `EventSequence`
equals the physical Event WAL record sequence and is not repeated in
`EventWalPayload`. An incomplete Event WAL tail without a final event is not a
complete command result.

One Event WAL belongs to one instrument and one epoch. `InstrumentId` and
`EpochId` are stream/file metadata and are not repeated in `EventWalPayload`.

`PublishStatus::Fatal` means the writer can no longer provide its publication
contract. The failing event may be definitely not accepted or may have unknown
publication status. The matcher stops immediately. Because the publication
channel is no longer reliable, `EventWriterFatal` diagnostics are available
through `RunResult` and out-of-band runtime/executor reporting, not through a
`MatcherFatal` event sent to the same writer. Recovery is performed by loading
the last valid snapshot and replaying the valid ordered command stream.
`CommandReaderFatal` is also out-of-band because it is not caused by an
accepted command and has no truthful `ClientId` or `CommandSequence`.

`OrderId` is assigned upstream before a command reaches the matcher. Within one
matcher epoch, each new-order command must satisfy `order_id > last_order_id_`.
After this check passes, the ID is consumed even if the order is later rejected
for business reasons. A stale, duplicate, or out-of-order `OrderId` is a fatal
ordered-stream invariant violation, not an `OrderRejected` event. The matcher
publishes a terminal `MatcherFatal` event for this case because the event writer
is still considered healthy. The matcher performs no active-book duplicate
lookup for a new order; the monotonicity check is the duplicate/stale guard.
`(EpochId, OrderId)` identifies an order globally; epoch infrastructure is
outside this sample.

`EventWriterFatal` is different: if publishing itself fails, the matcher cannot
reliably publish a fatal marker through the same writer, so diagnostics are only
available through `RunResult` and out-of-band runtime reporting.

Snapshot barriers use a caller-provided snapshot store only for matcher-local
state capture/load. After successful local capture/load, the matcher publishes a
`SaveSnapshot` or `LoadSnapshot` event through the same event writer used for
order events. Downstream stateful modules observe that event in Event WAL and
perform their own local barrier work. Save durability failures after the memory
handoff are handled by external recovery/runtime mechanisms. Failed local
capture, failed barrier publication, or invalid/unavailable local load is
terminal for the matcher.

Snapshot command payloads contain snapshot identity only: `snapshot_id` and
`snapshot_epoch_id`. The causal command sequence comes from the enclosing
`CommandEnvelope` and is copied into the common `EventWalPayload` as
`caused_by_command_sequence`; it is not repeated in the snapshot event body.
The loaded snapshot image carries its own source command boundary independently.

This is a base sample, not a complete exchange matching engine. Open design
items are tracked in [Matcher TODO.md](../../thoughts/exchange/matcher/Matcher%20TODO.md).

Canonical Command and Event WAL payload schema versions 1 and 2 are implemented
by `codec.hpp`; version 2 is current. Both use fixed 48-byte Command payloads
and fixed 64-byte Event payloads, explicit little-endian fields, validated
tags/enums, and a zeroed unused tail. Version 2 preserves every version-1 byte
layout and adds persisted `StartReplay`/`StopReplay` commands and events. The
exact byte layout is defined in
[WAL_PAYLOAD_FORMAT.md](WAL_PAYLOAD_FORMAT.md).

The base five-role runtime transport is implemented by `CommandPipeline` with
isolated `head`, `durable`, `risk_checked`, `reserve_checked`, and `tail`
frontiers. Its ownership, memory-ordering, backpressure, and fail-closed
persistence contracts are defined in
[COMMAND_PIPELINE.md](COMMAND_PIPELINE.md).

The deterministic replay cold path is exposed by `replay.hpp`:

- `WalFileReplaySource` validates and decodes a bounded Command WAL range and
  publishes original `CommandEnvelope` identities through `try_replay()`;
- `EventWalComparator` validates the corresponding Event WAL range and compares
  physical sequence plus exact canonical version-2 payload bytes;
- source backpressure retains the decoded command instead of advancing or
  dropping it;
- comparison stops at the first missing, extra, malformed, or different event.

## Benchmark

`bench_matcher` measures the current sample contract over prebuilt state:

- OrderBook-only equivalents;
- `Matcher::process()` with `NullEventWriter`;
- `Matcher::process()` with `CountingEventWriter`;
- `Matcher::run()` with reader-backed streams;
- `SinkEventWriter`, which keeps the old volatile `g_sink` writes inside
  `publish()` to expose instrumentation cost;
- round-robin interleaving of scenario runs;
- per-scenario warmup before measured latency/throughput passes;
- optional benchmark-thread CPU pinning through `--cpu N`;
- non-crossing limit orders that rest;
- aggressive limit orders that fully fill one maker;
- aggressive limit orders that partially fill one maker;
- `Matcher::run()` over a reader-backed full-fill command stream.

Build and run on Windows:

```powershell
cmake --build --preset windows-msvc-release --target bench_matcher
..\build\windows-msvc\matcher\Release\bench_matcher.exe --cpu 2
```

Replace `2` with a logical CPU that is present in the process affinity mask.

Useful options:

```text
--cpu N
--runs N
--batches N
--commands-per-batch N
--warmup-commands N
```

Use Release results. Debug numbers only verify the harness. The `run_*`
latency pass uses independent batch-sized matcher states so it can report
hundreds of batch samples without timing allocation, command generation,
liquidity preload, or matcher construction.
