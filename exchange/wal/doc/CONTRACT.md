# WAL Contract

## Public API

The runtime WAL string is independent of persistence:

```cpp
class WalCore {
public:
  OpenResult open(const WalRuntimeConfig& config) noexcept;
  PublishResult try_publish(std::span<const std::byte> payload) noexcept;
  AccessResult try_view(Position position) const noexcept;
  ReclaimStatus reclaim(Position end) noexcept;
  void close() noexcept;

  Position head() const noexcept;
  Position tail() const noexcept;
};

class PersistenceModule {
public:
  OpenResult open(const std::filesystem::path& path,
                  const PhysicalWalConfig& config) noexcept;
  bool append(const RecordView& record) noexcept;
  bool sync() noexcept;
  bool close() noexcept;
  bool failed() const noexcept;
};
```

`WalCore` owns bounded warmed storage and the intrinsic `head` and `tail`
boundaries. It performs no file operation and has no durable boundary or
persistence failure state. `reclaim(end)` accepts only monotonic exclusive
boundaries in `[tail, head]`; the composition is responsible for proving that
all mandatory readers have finished below `end`.

`WalRuntimeConfig` contains runtime storage and sequence fields only.
`PhysicalWalConfig` contains persisted identity and physical layout fields and
does not contain runtime capacity. `PersistenceModule` owns the selected live
physical writer and its terminal failure state. It does not own or publish a
frontier and does not select batches.

Generic stage mechanics are provided by `slider.hpp`:

```cpp
Progress progress(initial_exclusive_end);
WalHeadProgress upstream(wal);
Slider slider(wal, upstream, progress.writer(), module);

SliderResult result = slider.process_available();
Position visible_downstream = progress.reader().acquire();
```

Every progress value is an exclusive end: value `N` certifies completion of
positions `[0, N)`. `Progress` owns one cache-line-isolated atomic and exposes
one embedded read-only `Reader` capability and one embedded `Writer`
capability. A composition gives downstream stages only the reader. Each slider
holds its own writer and is the only runtime publisher for that frontier.

`process_available()` is one bounded synchronous call. It snapshots the
upstream exclusive end, asks `AcquirePolicy` for a consecutive subrange,
validates that the range starts at the slider's current position and does not
pass upstream, obtains each immutable `RecordView`, and calls
`module.process(record)`. The module returns `true` only after processing that
position is complete. Only then does the slider advance its current position
and apply `PublishPolicy`.

`PublishPolicy` returns `Hold`, `Publish`, or `Failed`; it never receives the
frontier writer. This lets a policy perform a synchronous module-level batch
completion operation before authorizing publication while keeping the slider
as the sole frontier publisher. The supplied `AvailableRangeAcquire` selects
the whole observed range, and `OnePositionPublish` publishes after each
successfully processed position.

The slider owns no thread, scheduling loop, wait/spin/yield behavior, runtime
registry, virtual dispatch, neighbor type, persistence operation, or snapshot
interpretation. Calling and retry cadence belongs to the composition. A
`ViewUnavailable` result indicates a violated upstream/retention composition
contract or lifecycle transition; the slider does not reclaim WAL storage.

The following class is a compatibility composition retained while slider
mechanics are introduced:

```cpp
class Wal {
public:
  OpenResult open(const std::filesystem::path& path,
                  const WalConfig& config) noexcept;

  PublishResult try_publish(std::span<const std::byte> payload) noexcept;
  DurabilityResult advance_durable(std::uint32_t batch_size) noexcept;
  ConsumeResult try_consume(std::span<std::byte> payload) noexcept;
  AccessResult try_view(Position position) const noexcept;

  CloseResult close() noexcept;

  bool is_open() const noexcept;
  const WalConfig& config() const noexcept;
  WalSnapshot snapshot() const noexcept;
};
```

`snapshot()` is diagnostic. Its frontiers are not mutable controls.
`Wal` composes one `WalCore`, one `PersistenceModule`, and a transitional
durable frontier. It preserves the existing three-role behavior and statuses;
new tract code should compose the core and modules directly.

The cold-path API in `reader.hpp` provides `WalReader` and `scan_wal()`.
`WalReader::open()` requires the expected persisted WAL configuration; runtime
capacity is ignored. `read_next()` is sequential and allocation-free after
open. It returns `Record` only after complete physical validation.

The file must be quiescent: no writer may append, synchronize, truncate, or
replace it while a reader or scanner is active. Validation proves physical
integrity, not the still-open runtime writer's durable frontier. After a crash,
the maximal contiguous CRC-valid prefix is the authoritative recovered WAL and
all of its records participate in replay and rebuild.

The reader validates file identity, format, header CRC, record header CRC,
contiguous sequence, payload CRC, record boundaries, and zero padding. Any
failure is terminal for that reader instance. It never skips or attempts to
resynchronize after a damaged record.

`scan_wal()` is read-only. It returns the longest trusted record prefix and its
ending file offset. Partial record header, payload, or padding is classified as
`IncompleteTail`; other integrity failures are classified as corruption. File
truncation and recovery mutation are outside this API.

The cold-path API in `recovery.hpp` provides
`recover_incomplete_tail(path, expected)`. The caller must own exclusive access
to the quiescent file for the entire operation. Recovery first performs the same
validated scan, and mutates the file only when that scan reports an incomplete
trailing record. It truncates to `last_valid_offset`, physically synchronizes
the file, and performs a complete validated rescan before returning
`RecoveryStatus::Recovered`.

The following conditions are always refused without mutation:

- incomplete or invalid file header;
- invalid complete record, including the final record;
- corruption in the middle of the file;
- sequence gap or duplicate;
- stream, epoch, manifest, physical format, payload schema, or layout mismatch.

`RecoveryResult` reports the original and recovered sizes, removed byte count,
trusted record count, last sequence, and trusted offset. A clean file returns
`Clean` without opening it for mutation. A truncate or physical-sync failure
returns `IoError`; callers must not infer successful durability from that
result. If synchronization fails after truncation, the reported current size
may already differ from the original size; the process must remain fail-closed
instead of treating a subsequent clean scan as proof of durable recovery.
Every complete record in the validated post-crash trusted prefix is part of
history regardless of whether a client received an acknowledgement. Batches
are live-writer append-and-sync units only; the physical format has no batch
commit records or commit markers. Client retry and ingress idempotency are
outside the WAL tail contract.

## Roles

For `WalCore`, one producer owns `try_publish()` and one composition reclaimer
owns `reclaim()`. Coordinated read-only users may call `try_view()` while the
retention precondition is maintained. `open()` and `close()` require all these
roles to be stopped.

For the compatibility `Wal`, one producer calls `try_publish()`, one durability writer calls
`advance_durable()`, and one consumer calls `try_consume()`. The three roles may run
concurrently on separate threads. A second caller for any role is outside the
contract.

`open()` and `close()` require all role threads to be stopped.

## Payload And Lifetime

Each WAL instance has one non-zero `payload_size`, one file-level
`payload_schema_version`, one stream identity, one epoch, one non-zero
`first_sequence`, and bounded non-zero runtime `capacity`. Payload bytes and
the meaning of the schema version are opaque to the WAL. Schema version `0` is
reserved for callers that do not declare an application payload schema.

Generic infrastructure WALs may use zero stream, epoch, and manifest identities.
Command and Event WALs require non-zero `stream_id`, `epoch_id`, and
`manifest_id`. Runtime `capacity` is not part of the physical file identity.

`payload_size` is fixed for the entire file. Physical records do not carry an
individual payload length. Application schemas that encode shorter logical
values into the fixed payload are responsible for deterministic initialization
of every remaining byte.

Publish input and consume output spans remain owned by the caller. These
operations finish their copies synchronously and never retain the span or
access caller memory after return.

## Retained Position View

`try_view(position)` is a non-blocking, allocation-free borrowed read of the
runtime WAL. `Position` is an absolute zero-based position, never a slot number
or a physical sequence. The result contains `ViewStatus` and a `RecordView`
with `position`, physical `sequence`, and the complete fixed-size
`std::span<const std::byte> payload`. It does not copy payload bytes or move any
frontier. The current storage has no additional per-position service fields or
stage pockets.

Status checks precede address calculation:

- `Closed`: the WAL is not open;
- `Reclaimed`: `position < tail`;
- `Unpublished`: `position >= head`;
- `Ok`: `tail <= position < head`.

Unsuccessful results contain an empty payload and zero position/sequence.
Status reflects the observed frontiers; publication may advance concurrently.
The operation acquires `head` before exposing producer-written bytes. It does
not require persistence success or consult `durable`: retained pending records
are accessible to persistence and other appropriately coordinated readers.
`try_consume()` retains its existing durable-only contract. A downstream stage
must separately acquire and obey its upstream frontier before using a view.

Coordinates remain:

```text
retained positions                  [tail, head)
physical sequence of position p     first_sequence + p
exclusive frontier after position p p + 1
exclusive frontier after sequence N N - first_sequence + 1
```

The last conversion applies to a sequence in this WAL. Publication checks
sequence exhaustion, so a successfully viewed position has a representable
physical sequence. Only the implementation maps `position % capacity` to a
block. A reclaimed absolute identity cannot be used to read its replacement
after ring wraparound.

**Caller-owned retention is a precondition, not a feature of the view.** Before
requesting a potentially accessible position, the caller must coordinate with
the sole reclaimer so that `tail` cannot pass that position during the call or
while any returned view is used. Multiple readers may borrow the same retained
position under that condition. The range checks do not pin storage, register a
reader, or protect against concurrent reclamation. Rechecking atomics does not
make an uncoordinated reader safe.

For the current API, `try_consume()` advances `tail`: callers must finish using
all views of its position before allowing that consume operation to reclaim it.
For a linear slider composition, the reclaimer may follow the final mandatory
published frontier only after all readers of those positions have finished.

The view expires when `tail` passes its position. All view users must also stop
and retire their views before `close()`, destruction, or a subsequent reopen.
The existing lifecycle operations do not track outstanding views; in particular
failure-close may discard the non-durable range without advancing `tail`.
Views and runtime positions from a previous open lifetime cannot be reused.

## Publish

`try_publish()` is non-blocking and allocation-free. On success it copies one
payload into the block at `head`, publishes `head + 1`, and returns physical
sequence `first_sequence + position`.

It returns `Full` when `head - tail == capacity`. Success does not mean the
payload is durable or visible to the consumer.

The persistence-free `WalCore` does not infer downstream failure. A composition
must stop production when its mandatory persistence module fails. The
compatibility `Wal` preserves this rule: after a durability I/O failure,
producer calls return `IoError` without publishing more data.

Exhausting the physical sequence domain puts the WAL into a distinct
fail-closed `SequenceExhausted` producer state; no wrapped sequence is
published. The durability writer and consumer may finish the already published
valid prefix before close reports `SequenceExhausted`.

## Advance Durable

`advance_durable(batch_size)` selects at most `batch_size` positions from
`[durable, head)`. An empty selection is a successful no-op and performs no
physical sync.

For a non-empty batch the physical writer:

1. appends every physical record in sequence order;
2. performs exactly one OS-level physical synchronization;
3. reports success to the ring.

Only then does the ring publish the batch end as the new `durable` frontier.

Physical synchronization is `FlushFileBuffers` on Windows, `fdatasync` on
POSIX, and `fsync` on macOS. Creating a file also synchronizes its initial
header; POSIX creation additionally synchronizes the parent directory entry.

Append or sync failure leaves `durable` unchanged and permanently puts producer
and durability operations into fail-closed `IoError` state.

## Consume

`try_consume()` is non-blocking and allocation-free. It returns `Empty` when
`tail == durable`, including when non-durable data exists above that frontier.

On success it copies the next durable payload, advances `tail`, and returns its
physical sequence. Advancing `tail` makes the block reusable; it is not a
durable consumer acknowledgement or domain checkpoint.

Consumer operation does not inspect the physical writer or I/O failure state. It
can drain the previously published durable range after a later I/O failure.

## Lifecycle

`open()` validates configuration, allocates and warms all ring storage, creates
and physically synchronizes a new WAL file, resets frontiers, and only then
publishes the working state. Creation is exclusive: an existing path returns
`OpenStatus::FileAlreadyExists` and the existing file is not modified. The live
writer still creates only a new WAL; cold-path incomplete-tail recovery is an
explicit, separate operation and does not reopen the writer.

`close()` does not consume, persist, or synchronize data. It returns
`PendingConsumption` while `tail != durable`, including after an I/O failure,
because the consumer may still drain the already durable range. It returns
`PendingDurability` while a healthy WAL has `durable != head`.

After an I/O failure and after the durable backlog is consumed, `close()`
releases resources and returns `IoError`; the non-durable range
`[durable, head)` is lost according to the fail-closed contract. A healthy
close succeeds only when:

```text
tail == durable == head
```

Destruction releases resources but never advances durability.

## Intentional Limits

- Existing WAL files cannot be reopened by the live `Wal` writer.
- Recovery removes only scanner-proven incomplete trailing records. It does not
  repair corruption or reopen an existing live writer.
- There is no segment rotation, compaction, or consumer checkpoint.
- Storage is one monolithic allocation, not an external block pool.
- The model is a three-stage SPSC frontier chain, not a broadcast SPMC tract.
- NUMA placement and CRC acceleration are not implemented.
