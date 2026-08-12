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
- non-crossing limit orders that rest;
- aggressive limit orders that fully fill one maker;
- aggressive limit orders that partially fill one maker;
- `Matcher::run()` over a reader-backed full-fill command stream.

Build and run on Windows:

```powershell
cmake --build --preset windows-msvc-release --target bench_matcher
..\build\windows-msvc\matcher\Release\bench_matcher.exe
```

Use Release results. Debug numbers only verify the harness.
