# OrderBook Portability

## Verified

The following matrix has been built and tested:

| Platform | Compiler | Configurations | Result |
|---|---|---|---|
| Windows x64 | MSVC 19.44 | `/W4` Debug and Release `/O2` | Component tests, randomized tests, quick/stress/100M soak passed. |
| Windows x64 | MSVC 19.44 AddressSanitizer | Release | Component tests and quick differential soak passed without ASan diagnostics. |

MSVC AddressSanitizer on this platform does not support leak detection. The
normal soak harness records process working set, while all runtime tracking
containers are explicitly bounded by capacity or a fixed trace size.

## Documented, Not Yet Verified

The code and CMake configuration are intended to be tested next on:

- Linux x86-64 with GCC;
- Linux x86-64 with Clang;
- Linux x86-64 with Clang ASan and UBSan.

These platforms are not claimed as supported until their builds and tests have
actually run. Reproducible commands are provided below.

GCC Debug and Release:

```sh
cmake -S . -B ../build/linux-gcc-debug -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=g++
cmake --build ../build/linux-gcc-debug
ctest --test-dir ../build/linux-gcc-debug --output-on-failure

cmake -S . -B ../build/linux-gcc-release -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=g++
cmake --build ../build/linux-gcc-release
ctest --test-dir ../build/linux-gcc-release --output-on-failure
../build/linux-gcc-release/order_book/test_order_book_soak --mode stress
```

Clang Debug and Release:

```sh
cmake -S . -B ../build/linux-clang-debug -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=clang++
cmake --build ../build/linux-clang-debug
ctest --test-dir ../build/linux-clang-debug --output-on-failure

cmake -S . -B ../build/linux-clang-release -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=clang++
cmake --build ../build/linux-clang-release
ctest --test-dir ../build/linux-clang-release --output-on-failure
```

Clang ASan and UBSan:

```sh
cmake -S . -B ../build/linux-clang-sanitize -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"
cmake --build ../build/linux-clang-sanitize
ASAN_OPTIONS=detect_leaks=1 \
  ctest --test-dir ../build/linux-clang-sanitize --output-on-failure
../build/linux-clang-sanitize/order_book/test_order_book_soak \
  --mode quick --seed 0xC0FFEE
```

## Platform Contract

- The implementation requires C++20. It uses standard `<bit>` operations such
  as `std::countl_zero` and `std::countr_zero`, not compiler intrinsics.
- A 64-bit `std::size_t` is the current effective requirement.
- 32-bit targets are not claimed as supported. Large `OrderCapacity` values can
  overflow power-of-two index sizing on a 32-bit `std::size_t`.
- There is no little-endian dependency. The implementation does not serialize
  object representations or reinterpret fields as byte sequences.
- TSan is not required for the stated contract: `OrderBook` is a single-writer
  component whose calls must be externally serialized. This is not a claim that
  concurrent unsynchronized access is safe.

## Configuration Limits

On a 64-bit target, capacity and price-range arithmetic is exact for the public
32-bit scalar domains:

- `OrderCapacity == UINT32_MAX` would request `2^33` index buckets;
- the full `PriceTick` range would request `2^26` segments per side.

Those values imply impractical allocations. They are type limits, not supported
deployment sizes. Construction is bounded by address space and available
memory and can throw `std::bad_alloc`.

The complete memory formulas and concrete x64 layout example are in
[DESIGN.md](DESIGN.md#memory-model).
