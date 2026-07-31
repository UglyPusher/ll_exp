/**
 * @file bench_order_pool.cpp
 * @brief Focused OrderPool initialization and block acquire/release benchmarks.
 */
#include <fexma/order_book/order_pool.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string_view>
#include <vector>

using namespace fexma::order_book;

namespace {

volatile std::uint64_t g_sink = 0;

struct Stats {
  std::uint64_t blocks{};
  std::uint64_t operations{};
  double mean_ns_per_block{};
  double mean_ns_per_op{};
  double blocks_per_second{};
  double ops_per_second{};
  double p50_ns_per_block{};
  double p90_ns_per_block{};
  double p99_ns_per_block{};
  double max_ns_per_block{};
};

template <typename Fn>
Stats run_blocks(std::uint64_t blocks, OrderCapacity block_size, Fn&& fn) {
  std::vector<double> samples;
  samples.reserve(static_cast<std::size_t>(blocks));
  double total_ns = 0.0;

  for (std::uint64_t block = 0; block < blocks; ++block) {
    const auto start = std::chrono::steady_clock::now();
    fn(block);
    const auto stop = std::chrono::steady_clock::now();
    const double elapsed =
        static_cast<double>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(stop - start)
                .count());
    total_ns += elapsed;
    samples.push_back(elapsed);
  }

  std::sort(samples.begin(), samples.end());
  const auto percentile = [&samples](double p) {
    const std::size_t index = static_cast<std::size_t>(
        (static_cast<double>(samples.size() - 1) * p) / 100.0);
    return samples[index];
  };

  const std::uint64_t operations =
      blocks * static_cast<std::uint64_t>(block_size);
  return {blocks,
          operations,
          total_ns / static_cast<double>(blocks),
          total_ns / static_cast<double>(operations),
          (static_cast<double>(blocks) * 1'000'000'000.0) / total_ns,
          (static_cast<double>(operations) * 1'000'000'000.0) / total_ns,
          percentile(50.0),
          percentile(90.0),
          percentile(99.0),
          samples.back()};
}

template <typename Fn>
double run_one_ms(Fn&& fn) {
  const auto start = std::chrono::steady_clock::now();
  fn();
  const auto stop = std::chrono::steady_clock::now();
  return static_cast<double>(
             std::chrono::duration_cast<std::chrono::nanoseconds>(stop - start)
                 .count()) /
         1'000'000.0;
}

void print_build_context(OrderCapacity capacity) {
#if defined(_MSC_VER)
  std::cout << "compiler: MSVC _MSC_VER=" << _MSC_VER
            << " _MSC_FULL_VER=" << _MSC_FULL_VER << '\n';
#else
  std::cout << "compiler: unknown\n";
#endif
#if defined(_M_X64)
  std::cout << "architecture: x64\n";
#elif defined(_M_IX86)
  std::cout << "architecture: x86\n";
#else
  std::cout << "architecture: unknown\n";
#endif
#if defined(NDEBUG)
  std::cout << "NDEBUG: defined\n";
#else
  std::cout << "NDEBUG: not defined\n";
#endif
  std::cout << "layout: sizeof(Order)=" << OrderPool::order_size()
            << " alignof(Order)=" << OrderPool::order_align()
            << " capacity=" << capacity
            << " block_sizes=64,4096,65536" << '\n';
}

void print_init(std::string_view name, double ms,
                WarmUpTouchStats stats = {}) {
  std::cout << name << ": ms=" << ms;
  if (stats.bytes != 0 || stats.pages != 0) {
    std::cout << " bytes=" << stats.bytes << " pages=" << stats.pages;
  }
  std::cout << '\n';
}

void print_runtime(std::string_view name, OrderCapacity block_size,
                   const Stats& stats) {
  std::cout << name << ": block_size=" << block_size
            << " blocks=" << stats.blocks << " ops=" << stats.operations
            << " mean_ns/block=" << stats.mean_ns_per_block
            << " mean_ns/op=" << stats.mean_ns_per_op
            << " blocks/s=" << stats.blocks_per_second
            << " ops/s=" << stats.ops_per_second
            << " p50_ns/block=" << stats.p50_ns_per_block
            << " p90_ns/block=" << stats.p90_ns_per_block
            << " p99_ns/block=" << stats.p99_ns_per_block
            << " max_ns/block=" << stats.max_ns_per_block << '\n';
}

OrderPool make_warmed_pool(OrderCapacity capacity) {
  OrderPool pool(capacity, OrderPool::Uninitialized{});
  pool.prefault_pages();
  pool.reset();
  return pool;
}

void bench_init(OrderCapacity capacity) {
  std::cout << "[init]\n";

  {
    std::optional<OrderPool> pool;
    const double ms = run_one_ms(
        [&pool, capacity] { pool.emplace(capacity, OrderPool::Uninitialized{}); });
    print_init("pool_allocate_uninitialized", ms);
    g_sink += pool->capacity();
  }

  {
    OrderPool pool(capacity, OrderPool::Uninitialized{});
    const double ms = run_one_ms([&pool] { pool.prefault_pages(); });
    print_init("pool_prefault", ms, pool.last_warm_up_stats());
    g_sink += pool.last_warm_up_stats().pages;
  }

  {
    OrderPool pool(capacity, OrderPool::Uninitialized{});
    pool.prefault_pages();
    const double ms = run_one_ms([&pool] { pool.reset(); });
    print_init("pool_reset", ms);
    g_sink += pool.free_count();
  }
}

void bench_bulk_acquire(OrderCapacity capacity, OrderCapacity block_size) {
  const std::uint64_t blocks = capacity / block_size;
  const std::uint64_t operations =
      blocks * static_cast<std::uint64_t>(block_size);

  OrderPool pool = make_warmed_pool(capacity);
  std::vector<OrderIndex> acquired(static_cast<std::size_t>(operations));
  std::size_t position = 0;

  const Stats stats = run_blocks(blocks, block_size, [&](std::uint64_t) {
    for (OrderCapacity i = 0; i < block_size; ++i) {
      acquired[position++] = pool.acquire();
    }
  });

  g_sink += acquired.front();
  g_sink += acquired[acquired.size() / 2];
  g_sink += acquired.back();
  print_runtime("pool_bulk_acquire_blocks", block_size, stats);
}

void bench_bulk_release(OrderCapacity capacity, OrderCapacity block_size) {
  const std::uint64_t blocks = capacity / block_size;
  const std::uint64_t operations =
      blocks * static_cast<std::uint64_t>(block_size);

  OrderPool pool = make_warmed_pool(capacity);
  std::vector<OrderIndex> acquired(static_cast<std::size_t>(operations));
  for (std::uint64_t i = 0; i < operations; ++i) {
    acquired[static_cast<std::size_t>(i)] = pool.acquire();
  }

  std::size_t position = 0;
  const Stats stats = run_blocks(blocks, block_size, [&](std::uint64_t) {
    for (OrderCapacity i = 0; i < block_size; ++i) {
      pool.release(acquired[position++]);
    }
  });

  g_sink += pool.free_count();
  print_runtime("pool_bulk_release_blocks", block_size, stats);
}

void bench_runtime(OrderCapacity capacity) {
  std::cout << "[runtime]\n";
  for (OrderCapacity block_size : {OrderCapacity{64}, OrderCapacity{4096},
                                   OrderCapacity{65536}}) {
    bench_bulk_acquire(capacity, block_size);
    bench_bulk_release(capacity, block_size);
  }
}

} // namespace

int main() {
  constexpr OrderCapacity capacity = 1'000'000;

  print_build_context(capacity);
  bench_init(capacity);
  bench_runtime(capacity);

  std::cout << "sink=" << g_sink << '\n';
  return 0;
}
