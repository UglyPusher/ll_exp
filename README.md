# ll.examples

Research playground for low-latency C++ experiments.

This repository is organized by topic:

- `utilities/` - small standalone low-latency probes and helper experiments.
- `exchange/` - exchange-related components and samples.
- `docs/` - topic-oriented design and reference documentation.
- `thoughts/` - topic-oriented notes, drafts, and open questions.

The previous root README is preserved as `README.md.old`.

## Build

The project uses CMake.

```powershell
cmake --preset windows-msvc
cmake --build --preset windows-msvc-debug
ctest --test-dir ../build/windows-msvc -C Debug --output-on-failure
```

Release build:

```powershell
cmake --build --preset windows-msvc-release
```

Build artifacts are expected to stay outside this repository, under `../build/`.
