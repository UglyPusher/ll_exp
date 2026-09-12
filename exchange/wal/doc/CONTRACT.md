# WAL Contract

## Public API

`RecordTape` is independent of persistence:

```cpp
class RecordTape {
public:
  RecordTapeOpenResult open(const RecordTapeConfig& config) noexcept;
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
  bool process(const RecordView& record) noexcept;
  bool append(const RecordView& record) noexcept;
  bool sync() noexcept;
  bool close() noexcept;
  bool is_open() const noexcept;
  bool failed() const noexcept;
};

class PersistenceSlider {
public:
  PersistenceSlider(const RecordTape& source, Frontier& durable,
                    PersistenceModule& persistence,
                    Position maximum_count = 0) noexcept;
  SliderResult process_available() noexcept;
  Position current() const noexcept;
  void reset_quiescent(Position initial) noexcept;
  void set_maximum_count(Position maximum_count) noexcept;
};
```

RecordTape value types are declared in `record_tape_types.hpp`; physical WAL
format and lifecycle types are declared in `types.hpp`. RecordTape headers do
not depend on physical WAL definitions. Tape `default_alignment` and physical
`wal_default_alignment` are independent defaults even though both currently
equal 64.

`RecordTape` owns bounded warmed storage and the intrinsic `head` and `tail`
boundaries. It performs no file operation and has no durable boundary or
persistence failure state. `reclaim(end)` accepts only monotonic exclusive
boundaries in `[tail, head]`; the composition is responsible for proving that
all mandatory readers have finished below `end`.

`RecordTapeConfig` contains only runtime storage fields: fixed payload size,
capacity, and allocation alignment.
`PhysicalWalConfig` contains persisted identity and physical layout fields and
does not contain runtime capacity. `PersistenceModule` owns the selected live
physical writer and its terminal failure state. It does not own or publish a
frontier and does not select batches. `process(record)` is its slider-facing
append operation. Persistence failure publication is atomic so the producer
role can stop after observing a terminal append or sync failure.

Ordinary stage mechanics are provided by `slider.hpp`:

```cpp
Frontier frontier(initial_exclusive_end);
Slider slider(tape, frontier, module); // upstream is RecordTape::head()

SliderResult result = slider.process_available();
Position visible_downstream = frontier.acquire();
```

Every frontier value is an exclusive end: value `N` certifies completion of
positions `[0, N)`. `Frontier` owns one cache-line-isolated atomic. A Slider
holds its own non-const frontier reference and is its only runtime publisher;
downstream stages receive a const reference and can only acquire it.

`process_available()` is one synchronous call. It snapshots the upstream
exclusive end, obtains every consecutive immutable `RecordView` through that
end, and calls `module.process(record)`. The module returns `true` only after
processing that position is complete. Only then does Slider publish the next
exclusive end to its own frontier.

The first stage reads `RecordTape::head()` directly. A later stage receives an
explicit `const Frontier&` as upstream. `PersistenceSlider` is separate because
it selects a bounded batch, appends every selected record, synchronizes the
complete batch once, and only then publishes the durable frontier. Append or
sync failure leaves durable progress unchanged and is terminal for the module.

The slider owns no thread, scheduling loop, wait/spin/yield behavior, runtime
registry, virtual dispatch, neighbor type, persistence operation, or snapshot
interpretation. Calling and retry cadence belongs to the composition. A
`ViewUnavailable` result indicates a violated upstream/retention composition
contract or lifecycle transition; the slider does not reclaim `RecordTape`
storage.

`NoOpModule` accepts every complete `RecordView` without changing application
state. It exists as the minimal module for composition tests. In the linear
bare pipeline, the composition may call
`tape.reclaim(no_op_frontier.acquire())` only after the slider call has
returned and all views from the reclaimed range are retired. Publishing the
module frontier alone does not release storage or remove producer backpressure.

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
outside the `RecordTape` tail contract.

## Roles

For `RecordTape`, one producer owns `try_publish()` and one composition reclaimer
owns `reclaim()`. Coordinated read-only users may call `try_view()` while the
retention precondition is maintained. `open()` and `close()` require all these
roles to be stopped.

## Payload And Lifetime

Each physical WAL instance has one non-zero `payload_size`, one file-level
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

Publish input spans remain owned by the caller. Publication finishes its copy
synchronously and does not retain the span or access caller memory after
return.

## Retained Position View

`try_view(position)` is a non-blocking, allocation-free borrowed read of the
`RecordTape`. `Position` is an absolute zero-based position, never a slot number
or a physical sequence. The result contains `ViewStatus` and a `RecordView`
with `position` and the complete fixed-size
`std::span<const std::byte> payload`. It does not copy payload bytes or move any
frontier. The current storage has no additional per-position service fields or
stage pockets.

Status checks precede address calculation:

- `Closed`: the `RecordTape` is not open;
- `Reclaimed`: `position < tail`;
- `Unpublished`: `position >= head`;
- `Ok`: `tail <= position < head`.

Unsuccessful results contain an empty payload and zero position.
Status reflects the observed frontiers; publication may advance concurrently.
The operation acquires `head` before exposing producer-written bytes. It does
not require persistence success or consult `durable`: retained pending records
are accessible to persistence and other appropriately coordinated readers.
A downstream stage must separately acquire and obey its upstream frontier
before using a view.

Coordinates remain:

```text
retained positions                  [tail, head)
exclusive frontier after position p p + 1
```

Only the implementation maps `position % capacity` to a block. A reclaimed
absolute identity cannot be used to read its replacement after ring wraparound.

**Caller-owned retention is a precondition, not a feature of the view.** Before
requesting a potentially accessible position, the caller must coordinate with
the sole reclaimer so that `tail` cannot pass that position during the call or
while any returned view is used. Multiple readers may borrow the same retained
position under that condition. The range checks do not pin storage, register a
reader, or protect against concurrent reclamation. Rechecking atomics does not
make an uncoordinated reader safe.

For a linear slider composition, the reclaimer may follow the final mandatory
published frontier only after all readers of those positions have finished.

The view expires when `tail` passes its position. All view users must also stop
and retire their views before `close()`, destruction, or a subsequent reopen.
The lifecycle operations do not track outstanding views. Views and runtime
positions from a previous open lifetime cannot be reused.

## Publish

`try_publish()` is non-blocking and allocation-free. On success it copies one
payload into the block at `head`, publishes `head + 1`, and returns the
published position.

It returns `Full` when `head - tail == capacity`. Success does not mean the
payload is durable or available to a downstream stage.

The persistence-free `RecordTape` does not infer downstream failure. A
composition must stop production when its mandatory persistence module fails.

If the exclusive `head` reaches the end of the `Position` domain,
`try_publish()` returns `PositionExhausted`; no wrapped position is published.
Downstream stages may finish the already published valid prefix.

## Persistence Progress

`PersistenceSlider::process_available()` selects at most its configured
maximum count from `[durable, head)`. An empty selection is a successful no-op
and performs no physical sync.

For a non-empty batch the physical writer:

1. appends every physical record in sequence order;
2. performs exactly one OS-level physical synchronization;
3. reports success to the ring.

Only then does the ring publish the batch end as the new `durable` frontier.

Physical synchronization is `FlushFileBuffers` on Windows, `fdatasync` on
POSIX, and `fsync` on macOS. Creating a file also synchronizes its initial
header; POSIX creation additionally synchronizes the parent directory entry.

Append or sync failure leaves `durable` unchanged and puts
`PersistenceModule` into its terminal failed state.

## Lifecycle

`RecordTape::open()` validates configuration, allocates and warms ring storage,
and resets its frontiers. `PersistenceModule::open()` creates and physically
synchronizes a new WAL file. Creation is exclusive: an existing path returns
`OpenStatus::FileAlreadyExists` and is not modified. Cold-path incomplete-tail
recovery is an explicit, separate operation and does not reopen the live
writer. The composition is responsible for stopping all roles and closing the
two components in a safe order.

## Intentional Limits

- Existing WAL files cannot be reopened by `PersistenceModule`.
- Recovery removes only scanner-proven incomplete trailing records. It does not
  repair corruption or reopen an existing live writer.
- There is no segment rotation, compaction, or consumer checkpoint.
- Storage is one monolithic allocation, not an external block pool.
- The model is a three-stage SPSC frontier chain, not a broadcast SPMC tract.
- NUMA placement and CRC acceleration are not implemented.
