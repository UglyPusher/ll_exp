# OrderBook Benchmarks

The benchmark programs are diagnostic harnesses for comparing implementations
on the same machine. They are not a source of portable latency promises.

## Targets

| Target | Scope |
|---|---|
| `bench_order_book` | Public API operations, fill cycles, best recompute, churn, and construction/first-touch. |
| `bench_order_id_index` | Probe behavior, hit/miss paths, clustered deletion, wrap-around, and long churn. |
| `bench_order_pool` | Pool construction, acquire/release blocks, FIFO append, and FIFO unlink positions. |
| `bench_matcher` | Minimal matcher command processing over the public OrderBook API. |

Build and run on Windows:

```powershell
cmake --preset windows-msvc
cmake --build --preset windows-msvc-release --target bench_order_book bench_order_id_index bench_order_pool bench_matcher
..\build\windows-msvc\order_book\Release\bench_order_book.exe
..\build\windows-msvc\order_book\Release\bench_order_id_index.exe
..\build\windows-msvc\order_book\Release\bench_order_pool.exe
..\build\windows-msvc\matcher\Release\bench_matcher.exe
```

Use Release `/O2` results. Debug numbers do not describe hot-path behavior.

## Measurement Rules

- Setup, data generation, and state restoration are outside timed regions.
- Construction is measured only by an explicitly named construction scenario.
- Latency is sampled as normalized batch time, not by timing every operation.
- Throughput uses a separate unsampled batch pass, so latency timer calls do not
  contaminate throughput.
- Each public API and index scenario performs at least five runs.
- Output includes run number, iterations, latency sample count, percentiles,
  and maximum.
- The public API harness prints compiler, optimization mode, CPU identifier,
  process affinity, and process priority before measurement.
- Control scenarios such as index find hit/miss and cluster length one are
  required when comparing two executables.
- Executables should be alternated on one pinned CPU when collecting A/B data.

Wall time on the currently used AMD machine is environment-sensitive. Frequency
management and competing processes have moved unchanged controls by amounts
larger than some proposed optimizations. Keep raw runs, reject comparisons whose
controls do not overlap reasonably, and do not transfer exact percentages to a
different machine.

## Public API Scenarios

`bench_order_book` includes:

- construction and first-touch at 100K capacity;
- insertion into an existing dense FIFO and into a new level;
- best Bid and Ask over mixed levels;
- `set_remaining` hit and miss;
- erasure at FIFO head, middle, tail, and the only order at a level;
- near/far best-segment recompute and a segment-distance sweep;
- partial-fill (`best + set_remaining`) and full-fill (`best + erase`) cycles;
- dense FIFO churn and mixed 85% occupancy churn.

The segment-distance sweep prints both `segment_count` and `segment_gap`, so a
change in lookup behavior can be distinguished from setup or configuration
changes.

## Matcher Scenarios

`bench_matcher` covers the current minimal matcher sample:

- non-crossing limit orders that enter the book as resting liquidity;
- aggressive limit orders that fully fill one resting maker;
- aggressive limit orders that partially fill one resting maker;
- `Matcher::run()` over a reader-backed full-fill command stream.

The harness reports batch-normalized `ns/command` and a separate throughput
pass. It measures the sample contract, including event publication calls into a
counting writer, not a durable WAL or network publisher.

## Index Scenarios

`bench_order_id_index` measures sequential and random IDs at 10%, 25%, and 50%
bucket occupancy. It covers find hit/miss, successful and duplicate insertion,
erase hit/miss, linear and wrap-around clusters of length 1 through 128, and a
10M-operation random churn scenario. Probe, repair-scan, and moved-bucket counts
are reported separately from wall time.

## Structural Results

The following conclusions are based on operation counts and algorithmic
structure, not on one machine's nanosecond percentages:

- successful `insert` was reduced from separate find and insert passes to one
  index insertion pass;
- successful `erase` was reduced from separate find and erase passes to one
  `erase_and_get` pass;
- deletion changed from a potentially quadratic cluster-repair algorithm to
  one linear backward-shift repair pass;
- far best-segment lookup changed from scanning individual segments to scanning
  64-segment occupancy words;
- allocation guards confirm zero allocations in `insert`, `best`,
  `set_remaining`, and `erase` after construction.

These results constrain complexity and memory traffic. They do not establish a
universal latency or throughput figure.

## Interpreting Percentiles

`p50`, `p99`, `p99.9`, and `p99.99` describe the harness's printed batch sample
population. Always inspect `latency_samples` before interpreting a high
percentile. A percentile whose sample population is too small is not useful,
even if the formatter prints it.

Keep generated output in the build directory or another results directory; raw
benchmark output is not tracked in Git.
