# OrderBook portability validation

## Supported validation targets

The intended validation matrix is Windows x64 and Linux x86-64 with a 64-bit
`std::size_t`. A 32-bit platform contract has not been established. In
particular, `OrderIdIndex::bucket_count_for()` can overflow its power-of-two
growth for large `OrderCapacity` values when `std::size_t` is 32 bits.

The public scalar domains permit `OrderCapacity == UINT32_MAX` and the full
`PriceTick` range, but those are type limits, not practical construction
limits. On a 64-bit target:

- `OrderCapacity == UINT32_MAX` requests `2^33` index buckets;
- the full price range contains `2^26` price segments per side;
- construction is limited by address space and allocator success and may throw
  `std::bad_alloc`.

Capacity arithmetic is exact on the supported 64-bit targets. Index bucket
counts are powers of two, and probing uses a mask. Price segmentation uses
shifts and masks. The implementation does not serialize object representations
or reinterpret bytes, so it has no little-endian dependency.

`std::countr_zero` and `std::countl_zero` require C++20 `<bit>`. No compiler
intrinsics are called directly; instruction selection and fallback code are the
responsibility of MSVC, GCC, or Clang.

## Reproducible Linux commands

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

The soak executable accepts `--mode quick|stress|soak`, `--operations N`,
repeatable `--seed N`, `--validation-interval N`, and
`--duration-seconds N`. `--scenario NAME` runs one named matrix entry and is
included in failure reproduction commands. Its throughput is diagnostic only
and is not a benchmark result.
