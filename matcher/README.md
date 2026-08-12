# fexma::matcher

`fexma::matcher` is a minimal matching-component sketch built on top of
`fexma::order_book`.

It owns an `OrderBook`, consumes commands from a caller-provided reader, and
publishes events through a caller-provided writer. The matcher does not create
threads, configure CPU affinity, know about WAL files, or own transport.

The hot-path contract is intentionally small:

- one matcher instance has one mutable matching state;
- commands are processed strictly sequentially;
- `CommandReader::read_next()` may return `Ok`, `Empty`, or `Fatal`;
- graceful shutdown is an ordered `CommandType::Shutdown` command;
- new-order `OrderId` values are strictly monotonically increasing within one
  matcher epoch;
- `EventWriter::publish()` returns only `Ok` or `Fatal`;
- temporary writer capacity pressure is handled inside the writer and is not
  observable by the matcher;
- after `Fatal`, the matcher instance must not continue processing commands.

`Empty` means the matcher remains in its polling loop and performs no state
transition. The concrete pause/backoff strategy is deliberately not modeled in
this sample.

`PublishStatus::Fatal` means the writer can no longer provide its publication
contract. The failing event may be definitely not accepted or may have unknown
publication status. The matcher stops immediately; diagnostics are an
out-of-band responsibility of the runtime/executor, and recovery is performed by
loading the last valid snapshot and replaying the valid command/event stream.

`OrderId` is assigned upstream before a command reaches the matcher. Within one
matcher epoch, each new-order command must satisfy `order_id > last_order_id_`.
After this check passes, the ID is consumed even if the order is later rejected
for business reasons. A stale, duplicate, or out-of-order `OrderId` is a fatal
ordered-stream invariant violation, not an `OrderRejected` event. The matcher
publishes a terminal `MatcherFatal` event for this case because the event writer
is still considered healthy. `(EpochId, OrderId)` identifies an order globally;
epoch infrastructure is outside this sample.

`EventWriterFatal` is different: if publishing itself fails, the matcher cannot
reliably publish a fatal marker through the same writer, so diagnostics are only
available through `RunResult` and out-of-band runtime reporting.

This is a base sample, not a complete exchange matching engine. Open design
items are tracked in [Matcher TODO.md](../Matcher%20TODO.md).

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
..\build\windows-msvc\matcher\Release\bench_matcher.exe --cpu 6
```

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
