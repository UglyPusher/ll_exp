/**
 * @file bench_order_pool.cpp
 * @brief Focused OrderPool and intrusive FIFO component benchmarks.
 */
#include <fexma/order_book/detail/order_pool.hpp>
#include <fexma/order_book/side_book.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string_view>
#include <vector>

using namespace fexma::order_book;

namespace {

using detail::OrderPool;

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
  std::cout << "layout: sizeof(Order)=" << sizeof(detail::Order)
            << " alignof(Order)=" << alignof(detail::Order)
            << " sizeof(PriceLevel)=" << sizeof(PriceLevel)
            << " alignof(PriceLevel)=" << alignof(PriceLevel)
            << " sizeof(PriceSegment)=" << sizeof(PriceSegment)
            << " alignof(PriceSegment)=" << alignof(PriceSegment)
            << " capacity=" << capacity
            << " block_sizes=64,4096,65536" << '\n';
}

void print_init(std::string_view name, double ms) {
  std::cout << name << ": ms=" << ms;
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

void bench_init(OrderCapacity capacity) {
  std::cout << "[init]\n";

  {
    const double ms = run_one_ms([capacity] {
      OrderPool pool(capacity);
      g_sink += pool.validate_freelist() ? 1 : 0;
    });
    print_init("pool_construct_ready", ms);
  }
}

void bench_bulk_emplace(OrderCapacity capacity, OrderCapacity block_size) {
  const std::uint64_t blocks = capacity / block_size;
  const std::uint64_t operations =
      blocks * static_cast<std::uint64_t>(block_size);

  OrderPool pool(capacity);
  std::vector<OrderIndex> acquired(static_cast<std::size_t>(operations));
  std::size_t position = 0;

  const Stats stats = run_blocks(blocks, block_size, [&](std::uint64_t) {
    for (OrderCapacity i = 0; i < block_size; ++i) {
      acquired[position++] = pool.emplace(i, i, i, i + 1, Side::Bid);
    }
  });

  g_sink += acquired.front();
  g_sink += acquired[acquired.size() / 2];
  g_sink += acquired.back();
  print_runtime("pool_bulk_emplace_blocks", block_size, stats);
}

void bench_bulk_release(OrderCapacity capacity, OrderCapacity block_size) {
  const std::uint64_t blocks = capacity / block_size;
  const std::uint64_t operations =
      blocks * static_cast<std::uint64_t>(block_size);

  OrderPool pool(capacity);
  std::vector<OrderIndex> acquired(static_cast<std::size_t>(operations));
  for (std::uint64_t i = 0; i < operations; ++i) {
    const auto value = static_cast<OrderIndex>(i);
    acquired[static_cast<std::size_t>(i)] =
        pool.emplace(value, value, value, 1, Side::Bid);
  }

  std::size_t position = 0;
  const Stats stats = run_blocks(blocks, block_size, [&](std::uint64_t) {
    for (OrderCapacity i = 0; i < block_size; ++i) {
      pool.release(acquired[position++]);
    }
  });

  g_sink += pool.validate_freelist() ? 1 : 0;
  print_runtime("pool_bulk_release_blocks", block_size, stats);
}

void bench_fifo_append_existing_level(OrderCapacity capacity,
                                      OrderCapacity block_size) {
  const std::uint64_t blocks = capacity / block_size;
  const std::uint64_t operations =
      blocks * static_cast<std::uint64_t>(block_size);

  OrderPool pool(capacity);
  SideBook<Side::Ask> asks(0, 127);
  std::vector<OrderIndex> acquired(static_cast<std::size_t>(operations));
  std::size_t position = 0;

  const Stats stats = run_blocks(blocks, block_size, [&](std::uint64_t) {
    for (OrderCapacity i = 0; i < block_size; ++i) {
      const auto id = static_cast<OrderId>(position + 1U);
      const OrderIndex slot = pool.emplace(id, id + 1000U, 42, 100, Side::Ask);
      asks.append(pool, slot);
      acquired[position++] = slot;
    }
  });

  g_sink += asks.best_order(pool);
  g_sink += asks.order_count();
  g_sink += asks.total_quantity();
  print_runtime("fifo_append_existing_level_blocks", block_size, stats);
}

enum class FifoPosition : std::uint8_t {
  Head,
  Middle,
  Tail
};

void bench_fifo_unlink(OrderCapacity capacity, OrderCapacity block_size,
                       FifoPosition position_to_remove) {
  const OrderCapacity group_capacity = capacity / 3U;
  const std::uint64_t blocks = group_capacity / block_size;
  const std::uint64_t operations =
      blocks * static_cast<std::uint64_t>(block_size);

  OrderPool pool(capacity);
  SideBook<Side::Bid> bids(0, 127);
  std::vector<OrderIndex> victims(static_cast<std::size_t>(operations));

  for (std::uint64_t group = 0; group < operations; ++group) {
    const auto id_base = static_cast<OrderId>(group * 3U + 1U);
    const OrderIndex head =
        pool.emplace(id_base, id_base + 1000U, 42, 10, Side::Bid);
    const OrderIndex middle =
        pool.emplace(id_base + 1U, id_base + 1001U, 42, 20, Side::Bid);
    const OrderIndex tail =
        pool.emplace(id_base + 2U, id_base + 1002U, 42, 30, Side::Bid);
    bids.append(pool, head);
    bids.append(pool, middle);
    bids.append(pool, tail);

    switch (position_to_remove) {
    case FifoPosition::Head:
      victims[static_cast<std::size_t>(group)] = head;
      break;
    case FifoPosition::Middle:
      victims[static_cast<std::size_t>(group)] = middle;
      break;
    case FifoPosition::Tail:
      victims[static_cast<std::size_t>(group)] = tail;
      break;
    }
  }

  std::size_t victim_index = 0;
  const Stats stats = run_blocks(blocks, block_size, [&](std::uint64_t) {
    for (OrderCapacity i = 0; i < block_size; ++i) {
      bids.remove(pool, victims[victim_index++]);
    }
  });

  g_sink += bids.best_order(pool);
  g_sink += bids.order_count();
  g_sink += bids.total_quantity();

  switch (position_to_remove) {
  case FifoPosition::Head:
    print_runtime("fifo_unlink_head_blocks", block_size, stats);
    break;
  case FifoPosition::Middle:
    print_runtime("fifo_unlink_middle_blocks", block_size, stats);
    break;
  case FifoPosition::Tail:
    print_runtime("fifo_unlink_tail_blocks", block_size, stats);
    break;
  }
}

void bench_runtime(OrderCapacity capacity) {
  std::cout << "[runtime]\n";
  for (OrderCapacity block_size : {OrderCapacity{64}, OrderCapacity{4096},
                                   OrderCapacity{65536}}) {
    bench_bulk_emplace(capacity, block_size);
    bench_bulk_release(capacity, block_size);
    bench_fifo_append_existing_level(capacity, block_size);
    bench_fifo_unlink(capacity, block_size, FifoPosition::Head);
    bench_fifo_unlink(capacity, block_size, FifoPosition::Middle);
    bench_fifo_unlink(capacity, block_size, FifoPosition::Tail);
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
