# Building And Testing

From the repository root:

```powershell
cmake -S . -B ../build/wal
cmake --build ../build/wal --config Debug --target test_wal_core test_wal_slider test_wal_bare_pipeline test_wal_frontier_ring test_wal_position_view test_wal_reader test_wal_recovery
ctest --test-dir ../build/wal -C Debug -R "^test_wal_(core|slider|bare_pipeline|frontier_ring|position_view|reader|recovery)$" --output-on-failure
```

The WAL target enables the repository's maximum standard warning set: `/W4`
and `/permissive-` on MSVC, or `-Wall -Wextra -Wpedantic` elsewhere.

The contract executables cover configuration, warmed aligned storage, hot-path
allocations, FIFO and wrap-around, durability batching, physical file format,
CRC and padding, injected append/sync failures, concurrent producer,
durability-writer, and consumer roles, and generic slider progress, range,
failure, and publication-policy behavior. The bare-pipeline executable covers
linear reclamation, stopped-stage backpressure, retained-slot stability,
wraparound, and concurrent producer/slider operation. The reader and recovery
executables cover validated scanning, corruption classification, conservative
incomplete-tail truncation, physical synchronization, and refusal without
mutation.
