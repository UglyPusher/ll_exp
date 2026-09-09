# Demo 006 — current context

Status: working context  
Updated: 2026-09-09
Repository: `UglyPusher/ll_exp`  
Branch: `demo/simple-snapshot`

## Purpose

This document preserves the current understanding reached while comparing the
existing WAL implementation, the old `CommandPipeline` prototype, and the
accepted FTTh design for application `001-exchange/006-simple-snapshot-demo`.

This is an implementation working note. It does not replace FTTh ADRs.

## Existing code

### WAL

Steps 1 through 5 now provide this static runtime structure:

```text
WalCore::head -> PersistenceSlider -> DurableF
                       |
                       v
              PersistenceModule

WalCore::tail <- composition reclaimer <- downstream sliders
```

`WalCore` has independent runtime lifecycle and no path, physical writer,
durable frontier, or persistence failure state. `PersistenceModule` has an
independent physical lifecycle, accepts immutable `RecordView` values, and
owns append/sync failure. Runtime capacity is absent from its
`PhysicalWalConfig`.

Persistence is now attached through the generic slider. `PersistenceSlider`
uses a bounded acquire policy, calls `PersistenceModule::process()` for every
immutable WAL position in the selected batch, performs one sync through its
publish policy, and publishes `DurableF` only after success.

The public `Wal` class remains as a compatibility facade. It statically owns
`WalCore`, `PersistenceModule`, `PersistenceSlider`, and the slider's durable
`Progress`, preserving the previous three-role API and failure behavior. The
old special `durable_frontier_` no longer exists.

Generic slider mechanics are now available in
`exchange/wal/include/fexma/wal/slider.hpp`.
`Progress` separates read-only and writer capabilities over one exclusive-end
frontier. `Slider` reads one upstream frontier, obtains immutable absolute WAL
views, synchronously invokes one statically bound module, and exclusively
publishes its own frontier according to compile-time acquire and publish
policies. Repeated execution, waiting, reclamation, and lifecycle remain
composition responsibilities.

The first bare composition is now implemented and tested as:

```text
WalCore::head -> NoOpSlider -> NoOpF -> composition reclaimer -> tail
```

`NoOpModule` supplies the minimal successful module. Publishing `NoOpF` and
advancing `tail` remain separate actions; the composition reclaims only after
the synchronous slider invocation has retired every view in the processed
range.

The first two stateful application modules now live in
`exchange/snapshot_demo`. `HashChainModule` maintains a deterministic ordered
digest; `BitAccumulatorModule` maintains a payload set-bit count and an
order-sensitive rolling fold. Both include absolute position, physical
sequence, payload size, and every payload byte in their transition, require
strictly consecutive positions, and fail closed on gaps, duplicates, or
reordering.

The tested full tract is now:

```text
head -> PersistenceSlider -> DurableF
     -> HashChainSlider -> HashF
     -> BitAccumulatorSlider -> BitF
     -> composition reclaimer -> tail
```

Before the extraction, the component in
[`exchange/wal`](../../../exchange/wal) was a well-tested monolithic
three-stage construction:

```text
Producer -> Persistence -> Consumer
   head       durable       tail
```

It owned:

- a bounded preallocated ring of fixed-size payload blocks;
- absolute `head`, `durable`, and `tail` frontiers;
- physical append and synchronization;
- canonical little-endian file and record formats;
- CRC validation;
- a sequential reader and scanner;
- conservative recovery of an incomplete physical tail;
- failure semantics and lifecycle.

The existing WAL tests were also compiled directly with GCC 13.3 during the
review and passed:

```text
test_wal_frontier_ring: PASS
test_wal_reader:        PASS
test_wal_recovery:      PASS
```

The low-level storage access remains:

```cpp
Storage::block_at_slot(std::uint32_t slot)
```

It is private and slot-based. Public module access now uses
`WalCore::try_view(Position)`, which validates the absolute position against
`tail` and `head` before returning immutable payload access.

### CommandPipeline

[`exchange/matcher/CommandPipeline`](../../../exchange/matcher/COMMAND_PIPELINE.md)
is a separate, hard-coded prototype:

```text
head -> durable -> risk_checked -> reserve_checked -> tail
```

It is not connected to the live WAL writer. The intended persistence bridge was
never implemented: an external component was expected to copy pending commands,
write and synchronize them, and then call `publish_durable()`.

`CommandPipeline` remains useful as a source of tested ideas:

- per-stage frontiers;
- single-writer progress;
- release/acquire publication;
- bounded backpressure;
- sidecar ownership;
- separation of ring position from command sequence;
- live/replay publication scenarios.

The class itself is not a target component for Demo 006 because its topology
and domain stages are manually embedded in its API.

## Current target model

The WAL is the common ordered string.

It intrinsically owns only two special boundaries:

```text
tail <= retained positions < head
```

- `head` publishes the end of produced data;
- `tail` publishes the end of reclaimed data and permits slot reuse.

Everything else is a module attached to the WAL through its own slider:

```text
head
  |
  v
PersistenceSlider
  |
  v
HashChainSlider
  |
  v
BitAccumulatorSlider
  |
  v
tail
```

The current monolithic `durable` frontier is therefore not a fundamental third
WAL boundary. It becomes the published frontier of `PersistenceSlider`.

For a linear composition:

```text
tail <= BitF <= HashF <= DurableF <= head
```

Data does not move from module to module. Every slider addresses the same
retained WAL position. What moves through the pipeline is permission to process
that position, expressed by the upstream frontier.

## Slider relationship

Each slider:

- has read-only access to the WAL;
- has read-only access to the frontier immediately upstream;
- owns its current position;
- is the sole writer of its published frontier;
- holds or references one concrete domain module;
- invokes that module synchronously in the current thread;
- does not own an execution loop or waiting strategy.

Conceptually:

```cpp
const Wal& wal;
const UpstreamProgress& upstream;
Module& module;
Position current;
OwnProgress& published;
```

In the implementation, `current` and every frontier are exclusive ends. A
value `N` means positions `[0, N)` have completed. The supplied
`OnePositionPublish` policy therefore authorizes publication of `p + 1` only
after the module successfully processes absolute position `p`.

A downstream slider does not know the type of the upstream module. It knows
only its read-only progress interface.

The slider obtains the complete data object for a position and passes it to the
module without interpreting its contents.

## WAL position access

Modules must address an absolute position or physical sequence, never a raw
ring slot. The mapping remains internal:

```text
slot = absolute_position % capacity
```

A safe read-only access operation must distinguish at least:

- position not yet published by `head`;
- position already reclaimed by `tail`;
- retained accessible position.

A returned view must remain valid until `tail` passes that position.

## Reclamation

A module never advances `tail` merely because it has processed a position.

For the strictly linear Demo 006 composition, the composition/reclaimer may
advance `tail` through the published frontier of the last mandatory slider.
Until then the slot remains retained for all downstream modules.

## Demo 006 scope

The first milestone contains:

- the existing WAL as the common ordered string;
- a persistence slider;
- `HashChainModule`;
- `BitAccumulatorModule`;
- one slider per module;
- a snapshot generation containing both module captures;
- a composition-level snapshot sink;
- bootstrap restore;
- continuation from `N + 1`;
- comparison with uninterrupted execution.

The first milestone intentionally excludes:

- Matcher and OrderBook;
- Risk and Reserve;
- Event WAL;
- live persisted `LoadSnapshot`;
- rewind of a running tract;
- global quiescence;
- multiple snapshot generations in flight;
- production snapshot format;
- production throughput claims.

Matcher, rebuild, and replay are later integration steps after the tract and
snapshot mechanics are proven.
