# OrderBook Tests

Ordinary CTest is the quick tier. It includes component and contract tests,
`test_randomized` in its default mode, and `test_order_book_soak --mode quick`.
Long-running modes are explicit commands and are not registered with CTest.

## Quick Tier

Build and run all Debug or Release tests on Windows:

```powershell
cmake --preset windows-msvc
cmake --build --preset windows-msvc-debug
ctest --test-dir ..\build\windows-msvc -C Debug --output-on-failure

cmake --build --preset windows-msvc-release
ctest --test-dir ..\build\windows-msvc -C Release --output-on-failure
```

The quick differential tiers are:

- `test_randomized`: 1K operations for each of 6 fixed seeds, 6K total;
- `test_order_book_soak --mode quick`: 600K operations across 10
  configurations and 3 fixed seeds.

## Explicit Stress and Soak

```powershell
# 50K operations for each of 6 seeds: 300K total
..\build\windows-msvc\order_book\Release\test_randomized.exe --stress

# 6M operations across the complete soak matrix
..\build\windows-msvc\order_book\Release\test_order_book_soak.exe --mode stress

# 100M operations across the complete soak matrix
..\build\windows-msvc\order_book\Release\test_order_book_soak.exe --mode soak
```

`--mode soak` defaults to 100M operations. `--operations N` replaces the mode's
operation budget. `--duration-seconds N` runs the selected matrix and then
extends near-capacity churn for at least the requested duration.

## Seeds and Reproduction

`test_randomized` always prints its seed, failing step, error code, and the last
32 commands on failure.

The soak harness accepts repeatable custom seeds and an optional named scenario:

```powershell
..\build\windows-msvc\order_book\Release\test_order_book_soak.exe `
  --operations 2000000 --seed 0xC0FFEE

..\build\windows-msvc\order_book\Release\test_order_book_soak.exe `
  --operations 200000 --seed 0xC0FFEE --scenario bitmap_word_boundary
```

On failure, the soak harness prints a directly runnable reproduction command
with the exact scenario, seed, and per-scenario operation count. Its trace is a
fixed ring containing the last 64 operations, so diagnostic memory does not grow
with soak duration.

## Validation and Allocation

The quick randomized test calls `validate_invariants()` after every operation.
The soak harness validates periodically according to mode and performs a final
full validation and drain for every seed/configuration run.

Targeted preflight checks cover failure atomicity, aggregate quantities above
`UINT32_MAX`, repeated construction/destruction, move construction and
assignment, and zero hot-path allocations after construction. The allocation
guard excludes `validate_invariants()`, which is permitted to allocate.

The soak throughput and elapsed time are environment-sensitive diagnostics, not
benchmark results.
