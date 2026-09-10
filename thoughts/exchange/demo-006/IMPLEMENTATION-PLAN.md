# Demo 006 — implementation plan

Status: IMPLEMENTED / awaiting milestone freeze  
Updated: 2026-09-10  
Target branch: `demo/simple-snapshot`

> This plan is complete. It is retained as the implementation record for the
> first Demo 006 milestone and is no longer a working plan. Further changes to
> Demo 006 must be implemented through separate plans.
>
> Current project state: implementation complete; awaiting independent review.
> The milestone remains awaiting freeze until that review is complete.

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
format, adapter, reader, scanner, recovery, and CRC behavior remain unchanged.

[Historical implementation details for Steps 1–10 remain as recorded in the completed plan.]

## Completion status

All first-milestone implementation steps and all Step 10 negative/stress
scenarios, including synthetic 1, 10, 100, and 500 MiB captures, are implemented.
The implementation has passed the recorded Windows/MSVC and Linux/GCC checks.

Independent review of the first Demo 006 milestone remains pending. Until that
review is complete, the milestone status is `awaiting milestone freeze`.

## Later work

The items below are not continuations of this implementation plan. Each must be
introduced and executed through a separate plan after the first milestone is
reviewed and frozen:

1. Formalize rebuild as snapshot load plus WAL suffix processing.
2. Add replay source and replay persistence policy.
3. Reuse existing replay comparison/checkpoint code.
4. Attach Matcher as a later concrete module.
5. Add Event WAL only after the command-side mechanics are stable.

These steps must not be allowed to expand the scope of the first Demo 006
milestone.
