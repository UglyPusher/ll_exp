# Demo 006 — implementation plan

Status: working plan  
Updated: 2026-09-09
Target branch: `demo/simple-snapshot`

## Objective

Build the FTTh Demo 006 application incrementally from existing parts while
preserving a working, testable system after every structural change.

The implementation order is:

```text
WAL core
-> generic slider mechanics
-> persistence as a slider
-> two stateful modules
-> snapshot semantics
-> bootstrap restore
-> later rebuild/replay integration
```

## Step 0 — freeze the baseline

Before restructuring:

1. Build the current branch.
2. Run all WAL tests.
3. Record compiler, configuration, and results.
4. Do not change file format, reader, scanner, or recovery semantics during the
   initial extraction.

Gate:

```text
Existing WAL tests pass unchanged.
```

Baseline recorded on 2026-09-06 at commit
`9ac9b906320ad4ab43d52d44badd3ba458d358fa`; working tree was clean.

- Windows / Visual Studio 2022 / MSVC 19.44.35215.0 / x64 / C++20 / Release.
- `cmake --preset windows-msvc`: passed.
- `cmake --build --preset windows-msvc-release`: passed (whole branch).
- `ctest --preset windows-msvc-release -R "^test_wal_(frontier_ring|reader|recovery)$"`:
  3/3 passed, 3.66 seconds total; existing tests unchanged.
- Individual times: frontier ring 3.30 s, reader 0.15 s, recovery 0.16 s.
- CMake/CTest were invoked from the installed Visual Studio CMake bin directory
  because they were absent from the Windows shell PATH.

### Approved first patch order

The owner approved adding the Step 2 absolute read-only position API before
the Step 1 persistence extraction. This provides the tested access boundary
needed for that extraction while retaining the existing three-role API.

The first patch adds `Position`, `RecordView`, `AccessResult`, and
`Wal::try_view()`, documents caller-owned retention and coordinate conversion,
and registers `test_wal_position_view`. Existing WAL test sources, physical
format, adapter, reader, scanner, recovery, and CRC contracts are unchanged.
Persistence extraction and generic slider implementation remain pending.

First patch status: IMPLEMENTED; independent review remains pending.

Verification on 2026-09-06, Windows / MSVC 19.44.35215.0 / x64 / C++20 / Release:

- `cmake --preset windows-msvc`: passed.
- `cmake --build --preset windows-msvc-release`: passed (whole branch).
- `ctest --preset windows-msvc-release -R "^test_wal_(frontier_ring|reader|recovery|position_view)$"`:
  4/4 passed, 4.12 seconds total.
- Individual times: frontier ring 3.30 s, reader 0.18 s, recovery 0.20 s,
  position view 0.38 s.
- The original three WAL test sources are unchanged.

The new test verifies closed/unpublished/reclaimed positions, first/middle/last
views, unchanged frontiers, pending access before durability, stability until
reclamation, repeated capacity-1/capacity-3 wraparound, identical input and
addresses for two readers concurrent with producer publication, non-default
and maximal physical sequences, maximal rejected positions, immutable payload
access, and allocation-free success/failure paths.

Both readers finish before reclamation; this verifies the documented retention
contract, not safety of uncoordinated readers. Other test suites, benchmarks,
additional toolchains, and sanitizers were not run.

## Step 1 — separate the WAL string from persistence

Retain in the WAL core:

- bounded storage;
- `head`;
- `tail`;
- producer publication;
- absolute-position-to-slot mapping;
- read-only access to a retained position;
- reclamation and capacity checks;
- producer sequence exhaustion handling.

Move out of the WAL core:

- `durable_frontier_`;
- `durable_slot_`;
- live physical writer ownership;
- `io_failed_` as persistence state;
- the persistence work currently performed by `advance_durable()`.

Do not discard:

- physical file format;
- `PhysicalWalAdapter`;
- `WalReader`;
- scanner;
- recovery;
- physical identity and CRC contracts.

Split lifecycle conceptually:

```cpp
wal.open(runtime_config);
persistence.open(path, physical_config);
```

Gate:

- producer can publish positions;
- retained positions can be read by absolute position;
- no slot is reused before `tail`;
- wraparound remains correct;
- hot path remains allocation-free.

Step 1 status: IMPLEMENTED on 2026-09-08; independent review remains pending.

Implemented:

- `WalCore` owns only warmed bounded storage, `head`, `tail`, producer
  publication, absolute-position views, reclamation, and producer sequence
  exhaustion;
- `WalRuntimeConfig` contains only runtime storage and sequence fields;
- `PersistenceModule` owns the selected physical writer and terminal I/O
  failure state;
- `PhysicalWalConfig` contains physical layout and persisted identity without
  runtime capacity;
- `Wal` remains a transitional compatibility composition over `WalCore` and
  `PersistenceModule`, preserving its existing three-role API and failure and
  lifecycle behavior;
- at Step 1 completion, `durable_frontier_` remained only in that compatibility
  composition; Step 5 subsequently replaced it with `PersistenceSlider`'s
  ordinary `Progress` frontier;
- `durable_slot_` was removed; compatibility persistence reads the core by
  absolute position;
- shared `valid_config()` logic moved to `config.cpp` without changing the
  physical configuration contract.

Verification on 2026-09-08, Windows / MSVC 19.44.35215.0 / x64 / C++20 /
Release:

- `cmake --preset windows-msvc`: passed;
- `cmake --build --preset windows-msvc-release`: passed for the whole branch;
- `ctest --preset windows-msvc-release -R "^test_wal_(core|frontier_ring|position_view|reader|recovery)$"`:
  5/5 passed, 3.50 seconds total;
- individual times: frontier ring 2.63 s, reader 0.25 s, recovery 0.18 s,
  position view 0.34 s, core 0.07 s.
- `ctest --preset windows-msvc-release`: 16/16 registered tests passed,
  6.79 seconds total, including `test_matcher_wal_stream` and the unchanged
  `CommandPipeline` tests.

`test_wal_core` verifies persistence-free open, bounded publish/view/reclaim,
invalid reclamation, wraparound, sequence exhaustion, concurrent producer and
reclaimer operation, independent core/persistence lifecycles, and compatibility
of the resulting physical file with the unchanged `WalReader`.

The existing physical format, physical adapter implementation, reader, scanner,
recovery, and CRC code were not changed. Benchmarks, additional toolchains, and
sanitizers were not run.

## Step 2 — provide safe position access

Add a read-only position API over the existing storage.

Conceptually:

```cpp
AccessResult try_view(Position position) const noexcept;
```

The API must:

- accept an absolute position or sequence, not a slot number;
- reject a position before `tail`;
- reject a position at or after `head`;
- return immutable access;
- define view lifetime through reclamation;
- prevent wraparound/ABA confusion.

`Storage::block_at_slot()` remains the low-level addressing primitive.

Gate:

- first, middle, last, reclaimed, unpublished, and wrapped positions are tested;
- two readers of a retained position observe identical immutable input.

Step 2 status: IMPLEMENTED in Stage 1; independent review remains pending.

The absolute-position API is `WalCore::try_view(Position)`. It returns an
immutable `RecordView`, rejects reclaimed and unpublished identities before
slot mapping, and documents caller-owned retention through reclamation. Its
contract and verification are recorded under the approved first patch above.

## Step 3 — implement generic slider mechanics

Implement one common stage-mechanics template parameterized by:

```text
WAL/view
UpstreamProgress
OwnProgress
Module
AcquirePolicy
PublishPolicy
```

The slider must:

1. observe the upstream frontier;
2. obtain an allowed consecutive range;
3. obtain each full WAL data object;
4. synchronously invoke its concrete module;
5. advance its current position only after successful processing;
6. publish its own frontier according to `PublishPolicy`.

The slider must not:

- inspect record/domain type;
- implement persistence;
- recognize `SaveSnapshot` or `LoadSnapshot`;
- own a worker thread;
- own polling, spin, yield, sleep, or scheduling policy;
- know the previous or next module type;
- perform snapshot file I/O.

Use typed progress roles or separate reader/writer capabilities so that each
frontier has exactly one writer.

Step 3 status: IMPLEMENTED on 2026-09-08; independent review remains pending.

Implemented in `exchange/wal/include/fexma/wal/slider.hpp`:

- `Progress` owns one cache-line-isolated atomic exclusive-end frontier and
  exposes distinct embedded `Reader` and `Writer` capabilities;
- `WalHeadProgress` adapts the core head as a read-only upstream frontier;
- `Slider` is parameterized by view source, upstream progress, own progress,
  module, acquire policy, and publish policy;
- `AvailableRangeAcquire` selects the consecutive range visible in one
  upstream observation;
- `OnePositionPublish` authorizes publication after each successful module
  call;
- a publish policy returns `Hold`, `Publish`, or `Failed`, while only `Slider`
  receives and invokes the own frontier writer;
- one `process_available()` call is bounded by one acquired range and owns no
  worker, polling/wait strategy, persistence, snapshot semantics, registry, or
  virtual dispatch.

`test_wal_slider` verifies strict range order, exclusive progress publication,
upstream limits, empty ranges, retry after module failure, whole-range deferred
publication, progress mismatch, upstream regression, invalid policy ranges,
reclaimed and unpublished views, and publication failure.

Verification on 2026-09-08, Windows / MSVC 19.44.35215.0 / x64 / C++20 /
Release:

- `cmake --preset windows-msvc`: passed;
- `cmake --build --preset windows-msvc-release`: passed for the whole branch;
- `ctest --preset windows-msvc-release -R "^test_wal_(core|slider|frontier_ring|position_view|reader|recovery)$"`:
  6/6 passed, 3.86 seconds total;
- `ctest --preset windows-msvc-release`: 17/17 registered tests passed,
  7.05 seconds total.

The physical WAL format, adapter, reader, scanner, recovery, and CRC contracts
were not changed. Step 4 remains responsible for the concurrent bare pipeline,
backpressure, reclamation, and wraparound proof.

## Step 4 — prove the bare pipeline

Create a trivial `NoOpModule`.

Composition:

```text
Producer -> head -> NoOpSlider -> tail
```

The composition/reclaimer advances `tail` only through the last published
module frontier.

Tests:

- strict ordering;
- no gaps or duplicates;
- wraparound;
- bounded backpressure;
- stopped slider stops reclamation and eventually the producer;
- producer and slider concurrency;
- slot contents remain stable until reclamation;
- final `tail == slider_frontier == head`.

Gate:

```text
A large deterministic command sequence passes from head to tail without loss,
duplication, reordering, or premature reuse.
```

Step 4 status: IMPLEMENTED on 2026-09-08; independent review remains pending.

Implemented:

- `NoOpModule` is a trivial statically bound module that completes every full
  `RecordView` synchronously;
- the bare composition wires `WalHeadProgress`, `Slider<NoOpModule>`, its own
  `Progress`, and composition-owned `WalCore::reclaim()` without another queue
  or payload copy;
- publication of the NoOp frontier and advancement of `tail` are distinct;
  only the composition reclaims after the slider invocation and its borrowed
  views have completed.

`test_wal_bare_pipeline` verifies deterministic bounded backpressure with a
stopped slider, continued producer blocking after module publication but before
reclamation, retained address and payload stability, slot reuse only after
reclamation, and wraparound. Its concurrent scenario sends 200,000 records
through capacity 127, checking every absolute position, physical sequence, and
payload for gaps, duplicates, reordering, or premature reuse. It finishes with:

```text
tail == NoOpF == head == 200000
```

Verification on 2026-09-08, Windows / MSVC 19.44.35215.0 / x64 / C++20 /
Release:

- `cmake --preset windows-msvc`: passed;
- `cmake --build --preset windows-msvc-release`: passed for the whole branch;
- `test_wal_bare_pipeline` passed 10 consecutive runs;
- `ctest --preset windows-msvc-release -R "^test_wal_(core|slider|bare_pipeline|frontier_ring|position_view|reader|recovery)$"`:
  7/7 passed, 3.58 seconds total;
- `ctest --preset windows-msvc-release`: 18/18 registered tests passed,
  5.62 seconds total.

The physical WAL format, adapter, reader, scanner, recovery, CRC contracts,
and transitional persistence compatibility composition were not changed.

## Step 5 — attach persistence through a slider

Attach the extracted `PersistenceModule` to the generic slider mechanics and
move the transitional durable frontier out of the compatibility composition.

Composition:

```text
Producer
  -> head
  -> PersistenceSlider
  -> durable frontier
  -> NoOpSlider
  -> tail
```

Persistence processing:

1. obtain positions from `[current, head)` using exclusive-end frontiers;
2. append the corresponding immutable WAL data;
3. complete the selected batch;
4. perform one physical synchronization;
5. publish the persistence frontier only after successful sync.

Required tests:

- downstream cannot pass the persistence frontier;
- empty batch performs no sync;
- one non-empty batch performs one sync;
- append failure does not publish progress;
- sync failure does not publish progress;
- persistence failure is fail-closed for further production;
- already published durable data can drain downstream;
- resulting files pass the existing reader/scanner;
- incomplete-tail recovery behavior is preserved;
- physical format and identity remain compatible.

Gate:

```text
The refactored pipeline reproduces the guarantees of the original monolithic
WAL.
```

Step 5 status: IMPLEMENTED on 2026-09-08; independent review remains pending.

Implemented:

- `PersistenceModule::process(RecordView)` supplies the ordinary slider module
  contract while retaining `append()` and `sync()` as persistence operations;
- `BoundedRangeAcquire` selects at most the requested batch size;
- `PersistenceBatchPublish` holds the frontier after each append, performs one
  sync after a complete non-empty range, and authorizes publication only after
  successful sync;
- `PersistenceSlider` is a static specialization of the generic `Slider` over
  `WalCore`, `WalHeadProgress`, `PersistenceModule`, and those two policies;
- `Wal` now statically owns that slider and its ordinary durable `Progress`;
  the special `durable_frontier_` implementation was removed while the legacy
  public API and statuses were preserved;
- persistence failure publication is atomic so the producer role observes the
  terminal failure without a data race;
- quiescent reset of `Progress` and `Slider` preserves facade reopen behavior
  and provides the initialization primitive later needed by bootstrap restore.

`test_wal_persistence_slider` proves the direct tract:

```text
head -> PersistenceSlider -> DurableF -> NoOpSlider -> tail
```

It verifies empty batches, bounded batches of 2/2/1, exactly one sync per
non-empty batch, downstream gating, append failure, sync failure, unchanged
durable progress on failure, durable-prefix draining, facade reopen, and
compatibility of the produced file with the unchanged `WalReader` and scanner.
The existing compatibility tests continue to verify fail-closed production,
durable-prefix consumption, physical layout/CRC, recovery, sequence exhaustion,
and concurrent producer/persistence/consumer roles.

Verification on 2026-09-08, Windows / MSVC 19.44.35215.0 / x64 / C++20 /
Release:

- `cmake --preset windows-msvc`: passed;
- `cmake --build --preset windows-msvc-release`: passed for the whole branch;
- `ctest --preset windows-msvc-release -R "^test_wal_(core|slider|bare_pipeline|persistence_slider|frontier_ring|position_view|reader|recovery)$"`:
  8/8 passed, 3.66 seconds total;
- `ctest --preset windows-msvc-release`: 19/19 registered tests passed,
  6.47 seconds total.

The physical writer implementation, file format, reader, scanner, recovery,
and CRC contracts were not changed.

## Step 6 — attach stateful modules

Implement:

- `HashChainModule`;
- `BitAccumulatorModule`.

Composition:

```text
head
  -> PersistenceSlider
  -> HashChainSlider
  -> BitAccumulatorSlider
  -> tail
```

Initial policies for both stateful stages:

```text
AvailableRangeAcquire
OnePositionPublish
```

Tests:

- `tail <= BitF <= HashF <= DurableF <= head`;
- BitAccumulator never passes HashChain;
- both modules process identical ordered WAL positions;
- changed payload changes terminal state;
- skipped, repeated, or reordered position changes terminal state;
- different stage speeds preserve correctness;
- reclamation follows only the last mandatory frontier.

Gate:

```text
Both modules deterministically reach M over the same WAL while occupying
different pipeline positions during execution.
```

Step 6 status: IMPLEMENTED on 2026-09-09; independent review remains pending.

Implemented in the separate `exchange/snapshot_demo` application component:

- `HashChainModule` maintains a deterministic 64-bit FNV-1a-style ordered
  digest over prior state, absolute position, physical sequence, payload size,
  and every payload byte;
- `BitAccumulatorModule` maintains total payload set bits and an order-sensitive
  rolling fold over the same record identity and payload;
- both modules require `record.position == processed_end`, complete
  synchronously without allocation, and become terminally failed on a gap,
  duplicate, or reordered position;
- both sliders use `AvailableRangeAcquire` and `OnePositionPublish`;
- the composition advances `tail` only through `BitF`, after the final slider
  invocation has retired its borrowed views.

`test_snapshot_demo_stateful_pipeline` verifies changed-payload sensitivity and
terminal rejection of skipped, repeated, and reordered positions. Its complete
tract sends 2,048 deterministic records through capacity 64 while persistence,
hash, and bit stages advance at different rates. Every cycle verifies:

```text
tail <= BitF <= HashF <= DurableF <= head
```

The test observes both `DurableF > HashF` and `HashF > BitF`, forces bounded
backpressure and wraparound, compares both terminal module states with
independent reference executions over the same records, and finishes with all
five frontiers at 2,048.

Verification on 2026-09-09, Windows / MSVC 19.44.35215.0 / x64 / C++20 /
Release:

- `cmake --preset windows-msvc`: passed;
- `cmake --build --preset windows-msvc-release`: passed for the whole branch;
- `test_snapshot_demo_stateful_pipeline` passed 10 consecutive runs;
- `ctest --preset windows-msvc-release`: 20/20 registered tests passed,
  7.66 seconds total.

The digest is an application consistency state, not a cryptographic
authenticator. Snapshot commands, captures, generation assembly, snapshot I/O,
and restore remain Step 7 and later work. Matcher, OrderBook, Risk, Reserve, and
Event WAL were not connected.

## Step 7 — introduce SaveSnapshot semantics

Add an application-specific record kind:

```text
SaveSnapshot
```

The slider remains unaware of this meaning.

For `SaveSnapshot` at position `N`, each stateful module must:

1. apply the normal deterministic transition for position `N`;
2. reach `StateAfter(N)`;
3. create an immutable module capture bound to generation `N`;
4. return successful completion;
5. only then allow the slider to publish frontier `N`.

Support exactly one capture generation in flight for the first milestone.

Gate:

- no capture before `StateAfter(N)`;
- both captures use the same logical boundary;
- one missing participant prevents complete generation publication.

Step 7 status: IMPLEMENTED on 2026-09-09; independent review remains pending.

Implemented in `exchange/snapshot_demo`:

- a canonical fixed 64-byte application codec distinguishes `Data` and
  `SaveSnapshot` without changing the physical WAL record format;
- a snapshot generation is the absolute position `N` carried by its
  `SaveSnapshot` record, and modules reject a command whose encoded generation
  differs from its WAL position;
- both stateful modules apply their normal transition through position `N`,
  increment their exclusive `processed_end` to `N + 1`, and only then create an
  immutable capture containing generation, position, physical sequence, and
  the complete `StateAfter(N)`;
- each module has one allocation-free capture slot; a later `SaveSnapshot`
  returns retryable failure without changing module state while that slot is
  occupied;
- `CaptureGenerationCoordinator` assembles an in-memory generation only after
  both mandatory captures agree on generation, position, exclusive boundary,
  and sequence;
- capture slots remain occupied until the composition releases the assembled
  generation, enforcing one generation in flight;
- the generic slider remains unaware of record kinds and publishes `N + 1`
  only after the module has returned success for position `N`.

`test_snapshot_demo_snapshot_semantics` verifies strict codec validation,
terminal malformed-record handling, `StateAfter(N)` capture ordering,
immutability while later data is processed, missing-participant and mismatch
handling, and retryable backpressure at a second snapshot. The existing
stateful pipeline test now uses valid encoded `Data` records.

Verification on 2026-09-09, Windows / MSVC 19.44.35215.0 / x64 / C++20 /
Release:

- `cmake --preset windows-msvc`: passed;
- `cmake --build --preset windows-msvc-release`: passed for the whole branch;
- `test_snapshot_demo_snapshot_semantics` passed 10 consecutive runs;
- `ctest --preset windows-msvc-release`: 21/21 registered tests passed,
  7.35 seconds total.

Snapshot file serialization, synchronization, final description publication,
and restore remain Step 8 and later work.

## Step 8 — implement the composition snapshot sink

Persist:

```text
snapshot-N/
    hash_chain.snapshot
    bit_accumulator.snapshot
    snapshot.description
```

The description is published last and contains at least:

- snapshot format version;
- epoch and WAL identity;
- application composition identity;
- snapshot sequence;
- required module identities;
- module snapshot schema versions;
- file sizes;
- checksums.

A directory without a valid final description or any required module file is
not a usable snapshot.

Measure independently:

- module capture;
- generation completion;
- serialization;
- write;
- flush;
- fsync;
- publication;
- total save latency.

Step 8 status: IMPLEMENTED on 2026-09-09; independent review remains pending.

Implemented in `exchange/snapshot_demo`:

- canonical little-endian schema-v1 encodings for the hash capture, bit
  accumulator capture, and complete generation description;
- the description records snapshot format version, stream/epoch/manifest WAL
  identity, application composition identity, generation and record boundary,
  physical snapshot sequence, required module identities, module schema
  versions, exact file sizes, and CRC32 checksums;
- the description carries its own CRC32 and reserved fields are required to be
  zero by the decoder;
- `SnapshotSink` writes and synchronizes both module files under
  `snapshot-N.pending`, writes and synchronizes the description last, and then
  publishes the generation by atomically renaming the directory to
  `snapshot-N`;
- stale staging directories are removed on retry, while an existing published
  generation is never overwritten;
- open, write, flush, fsync, close, and publication failures return explicit
  statuses and do not release the in-memory generation or module capture slots;
- composition-level `save_and_release()` releases the coordinator and both
  captures only after the complete directory has been durably published;
- save results expose module capture, generation completion, serialization,
  write, flush, fsync, publication, and total sink-call durations separately.

`test_snapshot_demo_snapshot_sink` verifies exact file membership, description
and module round trips, all required identities and checksums, corrupted
description rejection, publication-last interruption, stale-staging retry,
published-generation collision, and retained captures across injected open,
write, flush, and fsync failures.

Verification on 2026-09-09:

- Windows / MSVC 19.44.35215.0 / x64 / C++20 / Release full build: passed;
- `ctest --preset windows-msvc-release`: 22/22 registered tests passed,
  9.14 seconds total;
- `test_snapshot_demo_snapshot_sink` passed 10 consecutive runs;
- Linux / GCC 13.3 / x64 / C++20 / Release: all three Demo 006 tests passed.

Bootstrap selection, validation against expected runtime identity, isolated
state preparation, and atomic restore remain Step 9 work.

## Step 9 — implement bootstrap restore

Before worker threads start:

1. read and validate `snapshot.description`;
2. validate the complete required file set;
3. validate identities, schemas, sizes, and checksums;
4. prepare both module states in isolation;
5. publish neither state if any preparation fails;
6. publish the complete prepared composition;
7. initialize both slider positions/frontiers to `N`;
8. begin processing at `N + 1`.

Gate:

```text
continuous(1..M)
==
restore(snapshot@N) + process(N+1..M)
```

Compare both module terminal states and final progress values.

## Step 10 — negative and stress scenarios

Test at minimum:

- missing module capture;
- corrupted capture;
- incompatible module schema;
- incompatible WAL identity;
- captures from different boundaries;
- repeat of position `N`;
- omission of position `N + 1`;
- downstream progress beyond upstream;
- premature tail advancement;
- repeated snapshots at deterministic and randomized positions;
- synthetic module state sizes of 1, 10, 100, and 500 MiB.

## Later work

After the first milestone is frozen:

1. Formalize rebuild as snapshot load plus WAL suffix processing.
2. Add replay source and replay persistence policy.
3. Reuse existing replay comparison/checkpoint code.
4. Attach Matcher as a later concrete module.
5. Add Event WAL only after the command-side mechanics are stable.

These steps must not be allowed to expand the scope of the first Demo 006
milestone.
