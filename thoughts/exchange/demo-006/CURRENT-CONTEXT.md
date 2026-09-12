# Demo 006 — current context

Status: RecordTape refactor and Demo 006 acceptance complete
Updated: 2026-09-13
Repository: `UglyPusher/ll_exp`
Branch: `rnd/demo-006-refactor`
Production baseline commit: `9596ed3`

## Purpose

This is the canonical checkpoint for the accepted RecordTape, WAL, and
Snapshot Demo 006 boundaries. It records the as-built state needed to begin the
final RecordTape public API review without reconstructing decisions from chat
history. The historical implementation sequence remains in
`IMPLEMENTATION-PLAN.md`.

## Accepted component model

```text
RecordTape
    ordered bounded in-memory record storage

Record
    immutable after publication

Position
    absolute zero-based RecordTape position

head / tail
    intrinsic RecordTape publication and reclamation boundaries

TapeBoundary
    private cache-line-isolated RecordTape implementation detail

Frontier
    authoritative progress boundary of one consumer/module
```

`RecordTape` knows nothing about physical WAL sequence, `first_sequence`,
stream/epoch/manifest identity, persistence, WAL format, CRC, or snapshots.
Its public value types are declared in `record_tape_types.hpp`.

`RecordTape` exposes immutable retained records through
`try_view(Position)`. The implementation alone maps a position to a ring slot.
A view is valid only while the composition prevents `tail` from passing its
position. `head` moves monotonically after publication; `tail` moves
monotonically after all mandatory users have finished.

## Slider and Frontier

```text
Slider<Module>
    source RecordTape
    optional upstream Frontier
    own Frontier
    Module
```

The own `Frontier` is the only authoritative consumer progress. Slider does
not maintain a second current-position field: it acquires the current exclusive
end from its own Frontier, views records in order, invokes
`module.process(record)`, and publishes the next exclusive end only after the
module succeeds.

The first stage observes `RecordTape::head()` directly. Later stages observe a
read-only upstream Frontier. Slider owns no thread, polling policy, persistence,
snapshot logic, domain interpretation, or reclaim policy. The application owns
all objects and the execution schedule; Slider stores references.

Do not reintroduce the removed `Progress`, `Reader`, or `Writer` abstractions.

## Persistence and physical WAL

The live persistence path is:

```text
RecordTape
    -> PersistenceSlider
    -> PersistenceModule
    -> PhysicalWalAdapter
    -> file
```

`PersistenceSlider` is special consumer mechanics. It selects a bounded range,
passes each `RecordView` to `PersistenceModule`, performs one sync for the
complete non-empty batch, and publishes the durable Frontier only after sync
succeeds. It owns no file handle, WAL identity, sequence conversion, or format
logic.

`PersistenceModule` owns the live physical adapter and terminal persistence
failure state. It is the explicit coordinate boundary:

```text
physical_sequence = first_sequence + record.position
```

The addition is overflow-checked before the physical append. `first_sequence`
never flows back into RecordTape, Frontier, or Slider.

The physical WAL layer is accepted as clean for the current milestone. Its
responsibilities are:

- file and record headers;
- physical sequence encoding;
- alignment and zero padding;
- CRC validation;
- append and physical sync;
- physical byte reads;
- conservative incomplete-tail recovery.

`WalReader` reads persisted WAL records and tracks physical byte offsets.
`recover_incomplete_tail()` scans, truncates only a proven incomplete physical
tail, synchronizes it, and validates the retained file again. Neither component
contains application snapshot, rebuild, or replay behavior.

## Type boundary and alignment defaults

```text
record_tape_types.hpp
    Position
    ViewStatus / RecordView / AccessResult
    PublishStatus / PublishResult
    default_alignment

types.hpp
    physical format magic/version
    stream/epoch/manifest identity
    StreamKind
    WalConfig
    physical open status/result
    wal_default_alignment
```

RecordTape headers do not include the physical WAL type header. Physical WAL
code may consume a RecordTape `RecordView` only at the explicit persistence
boundary.

`default_alignment` and `wal_default_alignment` both currently equal 64. They
are independent defaults, not one shared contract: one configures RecordTape
storage and the other configures the persisted physical layout.

## Demo 006 acceptance topology

The application owns this static composition:

```text
RecordTape source
├── PersistenceSlider
│   ├── durable Frontier
│   └── PersistenceModule
├── Slider<HashChainModule>
│   ├── upstream: durable
│   ├── own: hash_frontier
│   └── HashChainModule
└── Slider<BitAccumulatorModule>
    ├── upstream: hash_frontier
    ├── own: bit_frontier
    └── BitAccumulatorModule
```

The application owns Tape, modules, Frontiers, Sliders, persistence, snapshot
coordinator, sink, and loader. Sliders store references and do not own Tape,
modules, or Frontiers. The verified linear invariant is:

```text
tail <= bit_frontier <= hash_frontier <= durable <= head
```

Demo 006 reaches RecordTape only through its public lifecycle, publication,
view/traversal, boundary, and reclamation contracts. It does not know ring
slots, `TapeBoundary`, allocation layout, or memory-ordering implementation.

## Snapshot capture and publication

A `SaveSnapshot` application record at RecordTape position `N` defines the
snapshot boundary. After each stateful module processes that record:

```text
record_position = N
processed_end   = N + 1
sequence        = first_sequence + N
```

Each module copies `StateAfter(N)` into its pending capture. The capture owns
its state and does not retain a `RecordView` or require the corresponding
RecordTape record to remain retained. A second snapshot marker applies
backpressure until the first capture is released.

`CaptureGenerationCoordinator` creates a generation only when both module
captures agree on generation, position, processed end, sequence, and captured
state boundary. `SnapshotSink` writes and synchronizes both module files and
the description in a staging directory, then publishes the completed directory
by rename. Captures are released only after successful publication.

## Snapshot load and quiescent restore

```text
read and validate description
    -> read and validate module snapshots
    -> prepare complete module states
    -> quiescent restore
    -> restore modules
    -> Slider::reset_quiescent(processed_end)
    -> continue suffix processing
```

Loading is isolated: no live state changes until the description and every
mandatory module capture pass identity, boundary, schema, size, checksum, and
encoding validation. `restore_snapshot_quiescent()` restores both module states
and their Slider Frontiers to the same exclusive `processed_end`. The caller
must stop all relevant roles before invoking it.

No snapshot-specific state belongs in RecordTape or Slider.

## Reclamation

The current linear Demo uses the last mandatory consumer as its safe boundary:

```cpp
source.reclaim(bit_frontier.acquire());
```

This is valid because `bit_frontier <= hash_frontier <= durable <= head`.
Snapshot captures contain copied state and therefore add no RecordTape
retention requirement. There is one application-level reclaimer. A generic
reclaim coordinator is neither implemented nor required for this topology.

## Completed refactor checkpoint

| Stage | Status |
|---|---|
| Legacy `Wal` facade removal | DONE |
| WAL inventory | DONE |
| Progress replaced by `Frontier` | DONE |
| RecordTape head/progress cleanup | DONE |
| `Slider<Module>` simplification | DONE |
| Generic/policy cleanup | DONE |
| `PersistenceSlider` separation | DONE |
| RecordTape semantic cleanup | DONE |
| RecordTape physical header boundary | DONE |
| Physical WAL review | DONE |
| Demo 006 acceptance review | DONE |
| Final RecordTape public API review | NEXT |
| Library extraction | PENDING |
| Final documentation | PENDING |

## Open questions / Next session

### Cold restart path

Production Demo 006 does not yet contain the complete restart path:

```text
process
    -> physical WAL
    -> snapshot
    -> process dies

new process
    -> physical WAL recovery
    -> load snapshot
    -> WalReader
    -> RecordTape population
    -> restored Slider topology
    -> suffix rebuild
```

Current acceptance proves snapshot load, module restore, Frontier restore, and
suffix Slider processing from an already available source. The missing disk
restart composition is an application-level orchestration gap, not a proven
RecordTape API defect.

### Coordinate origin

Before finalizing the public API, determine how positions in a new RecordTape
instance relate to snapshot `processed_end` when only a WAL suffix is loaded.
Do not choose an origin or add offset machinery without a concrete restart
contract.

### Durable Frontier after restart

Determine what the persistence `durable` Frontier means in a newly constructed
Tape and whether it must be restored. Current bootstrap restores only the two
stateful module Frontiers.

### `first_sequence` consistency

`first_sequence` is supplied independently to `PersistenceModule` and the
domain modules/verification logic. Consistency currently belongs to application
composition. Do not introduce shared context or configuration without a
separate decision.

### `WalConfig`

`WalConfig` still contains `capacity`, while `PersistenceModule` builds its
internal physical configuration with `capacity = 1`. This may be residue from
the older combined runtime-writer configuration. Leave it unchanged until a
real public-contract problem requires a decision.

## NEXT

Final RecordTape public API review.

Before changing the API, perform the cold-restart thought experiment:

```text
WalReader -> RecordTape -> restored Frontiers -> Slider suffix processing
```

Do not implement a restart framework unless that review exposes a concrete
missing contract.
