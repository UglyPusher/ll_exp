# Building And Testing

From the repository root:

```powershell
cmake -S . -B ../build/wal
cmake --build ../build/wal --config Debug --target test_wal_frontier_ring
ctest --test-dir ../build/wal -C Debug -R "^test_wal_frontier_ring$" --output-on-failure
```

The WAL target enables the repository's maximum standard warning set: `/W4`
and `/permissive-` on MSVC, or `-Wall -Wextra -Wpedantic` elsewhere.

The contract executable covers configuration, warmed aligned storage, hot-path
allocations, FIFO and wrap-around, durability batching, physical file format,
CRC and padding, injected append/sync failures, and concurrent producer,
durability-writer, and consumer roles.
