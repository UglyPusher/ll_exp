/**
 * @file bench_order_book.cpp
 * @brief Batch-normalized microbenchmarks and memory-layout report.
 *
 * Percentiles are computed from batch ns/op samples, not from one timestamp per
 * individual operation. The executable performs no OS affinity or memory
 * locking; environment tuning is external.
 */
#include <fexma/order_book/order_book.hpp>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

using namespace fexma::order_book;

namespace {

volatile std::uint64_t g_sink = 0;

struct Stats {
  std::uint64_t operations{};
  double mean{};
  double ops_per_second{};
  double p50{};
  double p90{};
  double p99{};
  double p999{};
  double p9999{};
  double max{};
  double avg_probes{};
  std::size_t max_probes{};
  std::size_t tombstones{};
};

template <typename Fn>
Stats run_batches(std::uint64_t batches, std::uint64_t ops_per_batch, Fn&& fn) {
  std::vector<double> samples;
  samples.reserve(static_cast<std::size_t>(batches));
  double total_ns = 0.0;
  for (std::uint64_t batch = 0; batch < batches; ++batch) {
    const auto start = std::chrono::steady_clock::now();
    for (std::uint64_t op = 0; op < ops_per_batch; ++op) {
      fn(batch, op);
    }
    const auto stop = std::chrono::steady_clock::now();
    const double elapsed =
        static_cast<double>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(stop - start)
                .count());
    total_ns += elapsed;
    samples.push_back(elapsed / static_cast<double>(ops_per_batch));
  }
  std::sort(samples.begin(), samples.end());
  const auto percentile = [&samples](double p) {
    const std::size_t index = static_cast<std::size_t>(
        (static_cast<double>(samples.size() - 1) * p) / 100.0);
    return samples[index];
  };
  const std::uint64_t operations = batches * ops_per_batch;
  return {operations,
          total_ns / static_cast<double>(operations),
          (static_cast<double>(operations) * 1'000'000'000.0) / total_ns,
          percentile(50.0),
          percentile(90.0),
          percentile(99.0),
          percentile(99.9),
          percentile(99.99),
          samples.back()};
}

void print(std::string_view name, const Stats& stats) {
  std::cout << name << ": ops=" << stats.operations << " mean_ns/op="
            << stats.mean << " ops/s=" << stats.ops_per_second
            << " p50=" << stats.p50 << " p90=" << stats.p90
            << " p99=" << stats.p99 << " p99.9=" << stats.p999
            << " p99.99=" << stats.p9999 << " max=" << stats.max;
  if (stats.avg_probes != 0.0 || stats.max_probes != 0 ||
      stats.tombstones != 0) {
    std::cout << " avg_probes=" << stats.avg_probes
              << " max_probes=" << stats.max_probes
              << " tombstones=" << stats.tombstones;
  }
  std::cout << '\n';
}

OrderBook make_book(OrderSlot capacity = 300000) {
  OrderBook book({1, 4096, capacity});
  book.warm_up();
  return book;
}

void put_checked(OrderBook& book, OrderId& id, Side side, PriceTick price,
                 Quantity quantity = 100) {
  const PutResult result = book.put({id, id + 1000, side, price, quantity});
  if (!result.ok()) {
    std::abort();
  }
  ++id;
}

void print_layout() {
  OrderBook sample({1, 4096, 100000});
  sample.warm_up();
  const std::size_t pool_bytes =
      OrderPool::order_size() * static_cast<std::size_t>(sample.capacity());
  const std::size_t index_bytes =
      OrderIdIndex::bucket_size() * sample.index_bucket_count();
  const std::size_t side_bytes =
      PriceSegment::segment_size() * sample.segment_count();
  std::cout << "layout: sizeof(Order)=" << OrderPool::order_size()
            << " alignof(Order)=" << OrderPool::order_align()
            << " sizeof(PriceLevel)=" << sizeof(PriceLevel)
            << " alignof(PriceLevel)=" << alignof(PriceLevel)
            << " sizeof(PriceSegment)=" << PriceSegment::segment_size()
            << " alignof(PriceSegment)=" << PriceSegment::segment_align()
            << " sizeof(OrderIdIndex::Bucket)=" << OrderIdIndex::bucket_size()
            << " alignof(OrderIdIndex::Bucket)="
            << OrderIdIndex::bucket_align() << '\n';
  std::cout << "memory: max_orders=" << sample.capacity()
            << " min_price_tick=1 max_price_tick=4096"
            << " segment_count=" << sample.segment_count()
            << " index_capacity=" << sample.index_bucket_count()
            << " index_load_factor="
            << static_cast<double>(sample.capacity()) /
                   static_cast<double>(sample.index_bucket_count())
            << " OrderPool_bytes=" << pool_bytes
            << " OrderIdIndex_bytes=" << index_bytes
            << " Bid_segments_bytes=" << side_bytes
            << " Ask_segments_bytes=" << side_bytes
            << " Total_bytes=" << (pool_bytes + index_bytes + side_bytes * 2)
            << '\n';
}

Stats with_probe_stats(Stats stats, const IndexProbeStats& probes,
                       std::uint64_t probe_ops, std::size_t tombstones) {
  stats.avg_probes =
      probe_ops == 0 ? 0.0
                     : static_cast<double>(probes.probes) /
                           static_cast<double>(probe_ops);
  stats.max_probes = probes.max_probe;
  stats.tombstones = tombstones;
  return stats;
}

} // namespace

int main() {
  print_layout();
  constexpr std::uint64_t fast_batches = 1000;
  constexpr std::uint64_t fast_ops = 1000;
  constexpr std::uint64_t book_batches = 300;
  constexpr std::uint64_t book_ops = 1000;

  {
    OrderPool pool(100000);
    pool.warm_up();
    print("pool_acquire", run_batches(fast_batches, fast_ops,
                                      [&](std::uint64_t, std::uint64_t) {
                                        g_sink += pool.acquire();
                                      }));
  }

  {
    OrderPool pool(100000);
    pool.warm_up();
    std::vector<OrderSlot> slots;
    slots.reserve(100000);
    for (int i = 0; i < 100000; ++i) {
      slots.push_back(pool.acquire());
    }
    std::size_t index = 0;
    print("pool_release", run_batches(fast_batches, 100,
                                      [&](std::uint64_t, std::uint64_t) {
                                        pool.release(slots[index++]);
                                      }));
  }

  for (double load : {0.25, 0.50, 0.70, 0.85}) {
    OrderIdIndex index(100000);
    const std::size_t active =
        static_cast<std::size_t>(static_cast<double>(index.bucket_count()) *
                                 load);
    for (std::size_t i = 0; i < active; ++i) {
      (void)index.insert(static_cast<OrderId>(i + 1),
                         static_cast<OrderSlot>(i));
    }
    IndexProbeStats probes{};
    const auto hit = run_batches(fast_batches, fast_ops,
                                 [&](std::uint64_t, std::uint64_t op) {
                                   const OrderId id =
                                       1 + static_cast<OrderId>(op % active);
                                   g_sink += index.find(id, &probes);
                                 });
    print("index_find_hit_load_" + std::to_string(static_cast<int>(load * 100)),
          with_probe_stats(hit, probes, fast_batches * fast_ops,
                           index.tombstone_count()));

    probes = {};
    const auto miss = run_batches(fast_batches, fast_ops,
                                  [&](std::uint64_t, std::uint64_t op) {
                                    g_sink += index.find(
                                        10'000'000 + static_cast<OrderId>(op),
                                        &probes);
                                  });
    print("index_find_miss_load_" + std::to_string(static_cast<int>(load * 100)),
          with_probe_stats(miss, probes, fast_batches * fast_ops,
                           index.tombstone_count()));
  }

  {
    OrderIdIndex index(600000);
    IndexProbeStats insert_probes{};
    OrderId id = 1;
    const auto stats = run_batches(fast_batches, fast_ops,
                                   [&](std::uint64_t, std::uint64_t) {
                                     (void)index.insert(id, static_cast<OrderSlot>(id),
                                                        &insert_probes);
                                     ++id;
                                   });
    print("index_insert", with_probe_stats(stats, insert_probes,
                                           fast_batches * fast_ops,
                                           index.tombstone_count()));
  }

  {
    OrderIdIndex index(600000);
    for (OrderId id = 1; id <= fast_batches * fast_ops; ++id) {
      (void)index.insert(id, static_cast<OrderSlot>(id));
    }
    IndexProbeStats erase_probes{};
    OrderId id = 1;
    const auto stats = run_batches(fast_batches, fast_ops,
                                   [&](std::uint64_t, std::uint64_t) {
                                     (void)index.erase(id++, &erase_probes);
                                   });
    print("index_erase", with_probe_stats(stats, erase_probes,
                                          fast_batches * fast_ops,
                                          index.tombstone_count()));
  }

  {
    OrderIdIndex index(128);
    for (OrderId id = 1; id <= 100; ++id) {
      (void)index.insert(id, static_cast<OrderSlot>(id));
    }
    IndexProbeStats probes{};
    OrderId next = 1000;
    const auto stats = run_batches(1000, 1000, [&](std::uint64_t, std::uint64_t op) {
      const OrderId victim = 1 + static_cast<OrderId>(op % 100);
      (void)index.erase(victim, &probes);
      (void)index.insert(next, static_cast<OrderSlot>(next), &probes);
      (void)index.erase(next, &probes);
      (void)index.insert(victim, static_cast<OrderSlot>(victim), &probes);
      ++next;
    });
    print("index_churn_after_1M_insert_erase",
          with_probe_stats(stats, probes, 4'000'000, index.tombstone_count()));
  }

  {
    auto book = make_book();
    OrderId id = 1;
    for (int i = 0; i < 10000; ++i) {
      put_checked(book, id, Side::Ask, 100);
    }
    print("select_best_only", run_batches(fast_batches, fast_ops,
                                          [&](std::uint64_t, std::uint64_t) {
                                            const auto best =
                                                book.select_best_opposite(Side::Bid);
                                            g_sink += best ? best->id : 0;
                                          }));
  }

  {
    auto book = make_book();
    OrderId id = 1;
    for (std::uint64_t i = 0; i < book_batches * book_ops; ++i) {
      put_checked(book, id, Side::Ask, 100, 1000);
    }
    print("decrement_partial_only", run_batches(book_batches, book_ops,
                                                [&](std::uint64_t, std::uint64_t) {
                                                  (void)book.select_best_opposite(
                                                      Side::Bid);
                                                  book.decrement_selected(1);
                                                }));
  }

  {
    auto book = make_book();
    OrderId id = 1;
    for (std::uint64_t i = 0; i < book_batches * book_ops; ++i) {
      put_checked(book, id, Side::Ask, 100, 100);
    }
    print("full_remove_keep_price_level_nonempty",
          run_batches(book_batches, book_ops, [&](std::uint64_t, std::uint64_t) {
            (void)book.select_best_opposite(Side::Bid);
            book.decrement_selected(100);
          }));
  }

  {
    auto book = make_book();
    OrderId id = 1;
    for (std::uint64_t i = 0; i < book_batches * book_ops; ++i) {
      put_checked(book, id, Side::Ask,
                  100 + static_cast<PriceTick>(i % prices_per_segment), 100);
    }
    print("full_remove_empty_price_level_same_segment",
          run_batches(book_batches, book_ops, [&](std::uint64_t, std::uint64_t) {
            (void)book.select_best_opposite(Side::Bid);
            book.decrement_selected(100);
          }));
  }

  {
    auto book = make_book();
    OrderId id = 1;
    for (PriceTick price = 64; price < 4096; price += 64) {
      put_checked(book, id, Side::Ask, price, 100);
    }
    print("full_remove_cross_segment",
          run_batches(63, 1, [&](std::uint64_t, std::uint64_t) {
            (void)book.select_best_opposite(Side::Bid);
            book.decrement_selected(100);
          }));
  }

  {
    auto book = make_book();
    OrderId id = 1;
    for (PriceTick price = 100; price < 164; ++price) {
      put_checked(book, id, Side::Ask, price, 100);
    }
    print("find_next_active_price_inside_segment_only",
          run_batches(64, 1, [&](std::uint64_t, std::uint64_t) {
            (void)book.select_best_opposite(Side::Bid);
            book.decrement_selected(100);
          }));
  }

  {
    auto book = make_book();
    OrderId id = 1;
    for (OrderId i = 0; i < book_batches * book_ops; ++i) {
      put_checked(book, id, Side::Bid, 100, 100);
    }
    OrderId cancel_id = 1;
    print("cancel_head", run_batches(book_batches, book_ops,
                                     [&](std::uint64_t, std::uint64_t) {
                                       const auto result = book.cancel(cancel_id++);
                                       g_sink +=
                                           static_cast<std::uint64_t>(result.status);
                                     }));
  }

  {
    auto book = make_book();
    OrderId id = 1;
    for (OrderId i = 0; i < book_batches * book_ops; ++i) {
      put_checked(book, id, Side::Bid, 100, 100);
    }
    OrderId change_id = 1;
    print("change_quantity", run_batches(book_batches, book_ops,
                                         [&](std::uint64_t, std::uint64_t) {
                                           const auto result =
                                               book.change(change_id++, {50});
                                           g_sink += static_cast<std::uint64_t>(
                                               result.status);
                                         }));
  }

  std::cout << "sink=" << g_sink << '\n';
  return 0;
}
