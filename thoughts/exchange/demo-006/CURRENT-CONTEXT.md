# Demo 006 — current context

Status: working context  
Updated: 2026-09-06  
Repository: `UglyPusher/ll_exp`  
Branch: `demo/simple-snapshot`

## Purpose

This document preserves the current understanding reached while comparing the
existing WAL implementation, the old `CommandPipeline` prototype, and the
accepted FTTh design for application `001-exchange/006-simple-snapshot-demo`.

This is an implementation working note. It does not replace FTTh ADRs.

## Existing code

### WAL

The existing component in [`exchange/wal`](../../../exchange/wal) is a
well-tested monolithic three-stage construction:

```text
Producer -> Persistence -> Consumer
   head       durable       tail
```

It currently owns:

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

The low-level storage access already exists:

```cpp
Storage::block_at_slot(std::uint32_t slot)
```

but it is private, slot-based, and has a mutable overload. It is not yet a safe
public contract for a module that addresses an absolute WAL position.

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
