# Demo 006 — design notes and invariants

Status: working decisions  
Updated: 2026-09-09

## Central idea

The WAL is both the bounded runtime storage and the single common ordered
string.

It has two intrinsic special boundaries:

```text
tail                                              head
  |                                                  |
  v                                                  v
------------------------------------------------------ WAL
       ^                 ^                 ^
       |                 |                 |
   Slider A          Slider B          Slider C
```

All processing stages are modules attached through sliders. There is no second
`CommandPipeline`, no queue between adjacent modules, and no separate runtime
tract storage.

## Data plane and progress plane

Data plane:

```text
one retained WAL position
+
immutable source record
+
optional stage-owned pockets
```

Progress plane:

```text
head -> F0 -> F1 -> ... -> Fn -> tail
```

The data object does not travel between modules. A frontier publication gives
the next slider permission to address the already existing WAL position.

## Core invariants

1. `head` and `tail` are intrinsic WAL boundaries.
2. Every intermediate frontier belongs to exactly one slider/stage.
3. Every frontier has exactly one writer.
4. A slider reads its upstream frontier but cannot modify it.
5. A slider cannot process beyond its upstream frontier.
6. Positions are processed strictly in order and without gaps.
7. Source record bytes are immutable after publication.
8. A module writes only its own state or designated pocket.
9. A slider publishes position `N` only after its module has completed
   processing through `N`.
10. Release publication of `N` makes all stage output through `N` visible to
    the acquire-reading downstream slider.
11. A retained position remains valid until `tail` passes it.
12. `head - tail <= capacity`.
13. Raw slot numbers are never used as public logical identities.
14. In normal execution all progress coordinates move monotonically forward.
15. For a strictly linear tract:
    ```text
    tail <= Fn <= ... <= F1 <= F0 <= head
    ```

## Ownership

| Object | Sole writer/owner | Readers |
|---|---|---|
| WAL `head` | Producer | First slider, diagnostics |
| Persistence frontier | PersistenceSlider | Next slider, diagnostics |
| Module frontier | Corresponding slider | Next slider, diagnostics |
| WAL `tail` | Composition/reclaimer | Producer, diagnostics |
| Source record | Producer before head publication | All permitted modules |
| Module pocket | Corresponding module | Permitted downstream modules |
| Domain state | Corresponding module | Snapshot capture logic |
| Snapshot generation | Composition snapshot coordinator | Snapshot sink |

## Slider contract

A slider combines mechanics with one concrete module:

```text
WAL access
+
read-only upstream progress
+
current position
+
own published progress
+
concrete module
+
acquire policy
+
publish policy
```

A slider is not:

- a module registry;
- a runtime graph node;
- a worker thread;
- an execution loop;
- a wait policy;
- a snapshot coordinator;
- a persistence backend;
- an interpreter of control records.

The slider and module execute synchronously in the same calling thread.

## Persistence interpretation

Persistence is not a third intrinsic WAL boundary.

It is the first ordinary attached stage for the live durable composition:

```text
upstream = head
module   = PhysicalPersistence
output   = durable frontier
```

Its special property is its domain contract:

```text
append complete batch
-> physical sync succeeds
-> publish frontier
```

The old `durable_frontier_` becomes this slider's published progress.

The physical reader, scanner, file format, and recovery remain WAL-related cold
path components. Extracting the live persistence stage does not invalidate or
replace them.

## Reclamation interpretation

`tail` is not the current position of an arbitrary consumer.

It is the WAL reclamation boundary.

For the first strictly linear Demo 006 composition, the reclaimer may follow
the published frontier of the last mandatory slider:

```text
tail <- BitAccumulator frontier
```

A later topology with branches or additional retained readers may require a
different composition-level reclaim policy. That question is outside the first
milestone.

## Static composition

Conceptually the application owns a heterogeneous compile-time sequence:

```cpp
std::tuple<
    PersistenceSlider,
    HashChainSlider,
    BitAccumulatorSlider
>;
```

Wiring is static:

```text
Persistence reads head
HashChain reads Persistence frontier
BitAccumulator reads HashChain frontier
Reclaimer reads BitAccumulator frontier
```

Modules do not know their neighbors and do not call downstream modules.

## Position versus slot

Internal storage may use:

```text
slot = position % capacity
```

Public and cross-component contracts use absolute positions or physical
sequences.

Before returning a position view, WAL must establish that it is:

```text
not reclaimed
and
already published
```

The view lifetime ends when reclamation passes the position.

## Snapshot semantics

`SaveSnapshot(N)` is an ordinary ordered application record.

For every mandatory stateful module:

```text
process position N
-> StateAfter(N)
-> immutable Capture@Module@N
-> module completion
-> slider may publish N
```

The complete application snapshot is:

```text
CaptureGeneration(N)
    = HashCapture@N
    + BitAccumulatorCapture@N
```

Physical snapshot I/O belongs to the composition-level sink, not to a module,
slider, or WAL core.

The first milestone uses bootstrap restore:

```text
validate complete snapshot
-> prepare every module state
-> publish the complete composition
-> set slider positions/frontiers to N
-> begin at N + 1
```

A failure to prepare any mandatory participant is a failure to load the entire
snapshot.

## Current resolved choices

- One common WAL string.
- No intermediate stage queues.
- Static compile-time module composition.
- One slider per module.
- Read-only reference to upstream progress.
- One writer of each published frontier.
- Full data object passed opaquely to the module.
- Slider and module execute synchronously.
- `AvailableRangeAcquire` and `OnePositionPublish` for the first stateful
  Demo 006 stages.
- Two stateful modules: `HashChainModule` and `BitAccumulatorModule`.
- `HashChainModule` uses a deterministic non-cryptographic ordered digest over
  record identity and payload bytes.
- `BitAccumulatorModule` keeps total payload set bits plus an order-sensitive
  rolling fold over record identity and payload bytes.
- Both modules fail closed unless each input absolute position equals their
  current exclusive processed end.
- A fixed canonical 64-byte application record distinguishes `Data` and
  `SaveSnapshot`; a snapshot generation equals its record's absolute position.
- Snapshot generation files use canonical little-endian Demo 006 schema v1;
  the checksummed description identifies the WAL, composition, generation,
  required modules, schemas, sizes, and file checksums.
- One snapshot generation in flight.
- Snapshot state is `StateAfter(N)`.
- Bootstrap restore for the first milestone.
- Matcher is not part of the first milestone.

## Questions deliberately left for implementation or later work

- Exact C++ names and concepts for typed progress readers/writers.
- Whether position access returns a reference, span, or small view object.
- Exact ownership representation for stage pockets.
- Exact acquire/publish policy interfaces.
- Worker wait strategy and CPU affinity.
- Reclaim policy for non-linear topology.
- Live persisted `LoadSnapshot`.
- Multiple snapshot generations in flight.
- Production batching and performance tuning.
- Matcher, Event WAL, rebuild protocol, and verification replay integration.

## Explicit non-goals

Do not:

- connect the old `CommandPipeline` to the refactored WAL;
- introduce runtime module registration;
- add virtual dispatch between slider and its module;
- put execution-loop mechanics inside the slider;
- teach the slider to interpret `SaveSnapshot`;
- put snapshot file I/O inside a module or slider;
- persist runtime atomic/frontier objects as module state;
- add Matcher merely to make the first demo look more exchange-like;
- redesign the physical WAL format during the initial extraction.
