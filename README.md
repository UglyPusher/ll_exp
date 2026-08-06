# ll.examples

A small research playground for low-latency C++ experiments.

This is not a library and not a framework. The repository is a collection of
sketches, probes, benchmarks, and small validation stands: a place to try an
idea, check invariants, measure an operation, compare implementation shapes, and
leave enough code behind to revisit the thought later.

## What's Inside

- `affinity.cpp` - experiments around thread affinity.
- `cache_locality.cpp` - simple checks for locality and cache-friendly access
  patterns.
- `numa.cpp` - early NUMA-related probes.
- `tp.cpp` - a tiny standalone example.
- `order_book/` - a more structured experiment with fixed-capacity order-book
  storage: intrusive FIFO levels, a preallocated order pool, a fixed
  `OrderId -> OrderIndex` index, tests, and microbenchmarks.

See [`order_book/README.md`](order_book/README.md) for details on the order-book
experiment.

## Principles

- Code here is written as working research notes, not as a stable public API.
- The experiments favor explicit data structures, fixed memory, and predictable
  runtime behavior.
- Tests and benchmarks exist to catch regressions in ideas, not to produce
  polished headline numbers.
- Some files may be rough, one-sided, or tied to a specific OS/compiler setup.

## Build

The project uses CMake.

Current local setup is Windows with Visual Studio 2022:

```powershell
cmake --preset windows-msvc
cmake --build --preset windows-msvc-debug
ctest --test-dir ../build/windows-msvc -C Debug --output-on-failure
```

Release build:

```powershell
cmake --build --preset windows-msvc-release
```

Linux build support is planned, but it is not documented as a supported path
yet.

## Status

This repository is a lab. Small programs, microbenchmarks, throwaway
implementations, and focused data-structure experiments all belong here when
they help reason about performance, memory, and data layout.

