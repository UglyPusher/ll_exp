/**
 * @file bench_order_book.cpp
 * @brief Public-API OrderBook benchmark harness.
 *
 * The benchmark measures only OrderBook's external five-method API. Setup is
 * performed before timed regions, except for the explicit construction
 * benchmark. Percentiles are computed from per-batch ns/op samples.
 */
#include <fexma/order_book/order_book.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

using namespace fexma::order_book;

namespace {

volatile std::uint64_t g_sink = 0;

struct Stats {
  std::uint64_t iterations{};
  std::uint64_t latency_samples{};
  double mean{};
  double ops_per_second{};
  double p50{};
  double p90{};
  double p99{};
  double p999{};
  double p9999{};
  double max{};
};

struct BatchShape {
  std::uint64_t batches;
  std::uint64_t ops_per_batch;
};

constexpr OrderBookConfig default_config{1, 4096, 400000};
constexpr BatchShape steady_shape{300, 1000};
constexpr int benchmark_runs = 5;

template <typename SetupFn, typename OperationFn>
Stats measure_scenario(BatchShape shape, SetupFn&& setup,
                       OperationFn&& operation) {
  std::vector<double> samples;
  samples.reserve(static_cast<std::size_t>(shape.batches));
  double total_ns = 0.0;

  {
    auto state = setup();
    for (std::uint64_t batch = 0; batch < shape.batches; ++batch) {
      const auto start = std::chrono::steady_clock::now();
      for (std::uint64_t op = 0; op < shape.ops_per_batch; ++op) {
        operation(state, batch, op);
      }
      const auto stop = std::chrono::steady_clock::now();
      const double elapsed =
          static_cast<double>(
              std::chrono::duration_cast<std::chrono::nanoseconds>(stop - start)
                  .count());
      total_ns += elapsed;
      samples.push_back(elapsed / static_cast<double>(shape.ops_per_batch));
    }
  }

  const std::uint64_t iterations = shape.batches * shape.ops_per_batch;
  double throughput_ns = 0.0;
  {
    auto state = setup();
    const auto start = std::chrono::steady_clock::now();
    for (std::uint64_t batch = 0; batch < shape.batches; ++batch) {
      for (std::uint64_t op = 0; op < shape.ops_per_batch; ++op) {
        operation(state, batch, op);
      }
    }
    const auto stop = std::chrono::steady_clock::now();
    throughput_ns =
        static_cast<double>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(stop - start)
                .count());
  }

  std::sort(samples.begin(), samples.end());
  const auto percentile = [&samples](double p) {
    const std::size_t index = static_cast<std::size_t>(
        (static_cast<double>(samples.size() - 1) * p) / 100.0);
    return samples[index];
  };
  return {iterations,
          shape.batches,
          total_ns / static_cast<double>(iterations),
          (static_cast<double>(iterations) * 1'000'000'000.0) / throughput_ns,
          percentile(50.0),
          percentile(90.0),
          percentile(99.0),
          percentile(99.9),
          percentile(99.99),
          samples.back()};
}

Stats run_construction_first_touch(std::uint64_t runs,
                                   const OrderBookConfig& config) {
  std::vector<double> samples;
  samples.reserve(static_cast<std::size_t>(runs));
  double total_ns = 0.0;

  for (std::uint64_t run = 0; run < runs; ++run) {
    const auto start = std::chrono::steady_clock::now();
    OrderBook book(config);
    const auto stop = std::chrono::steady_clock::now();
    const double elapsed =
        static_cast<double>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(stop - start)
                .count());
    total_ns += elapsed;
    samples.push_back(elapsed);
    const auto empty_bid = book.best(Side::Bid);
    g_sink += empty_bid ? empty_bid->id : 1;
  }

  double throughput_ns = 0.0;
  {
    const auto start = std::chrono::steady_clock::now();
    for (std::uint64_t run = 0; run < runs; ++run) {
      OrderBook book(config);
      const auto empty_bid = book.best(Side::Bid);
      g_sink += empty_bid ? empty_bid->id : 1;
    }
    const auto stop = std::chrono::steady_clock::now();
    throughput_ns =
        static_cast<double>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(stop - start)
                .count());
  }

  std::sort(samples.begin(), samples.end());
  const auto percentile = [&samples](double p) {
    const std::size_t index = static_cast<std::size_t>(
        (static_cast<double>(samples.size() - 1) * p) / 100.0);
    return samples[index];
  };
  return {runs,
          runs,
          total_ns / static_cast<double>(runs),
          (static_cast<double>(runs) * 1'000'000'000.0) / throughput_ns,
          percentile(50.0),
          percentile(90.0),
          percentile(99.0),
          percentile(99.9),
          percentile(99.99),
          samples.back()};
}

void print_stats(std::string_view name, int run, const Stats& stats) {
  std::cout << "baseline name=" << name << " run=" << run
            << " iterations=" << stats.iterations
            << " latency_samples=" << stats.latency_samples
            << " mean_ns/op=" << stats.mean
            << " ops/s=" << stats.ops_per_second << " p50=" << stats.p50
            << " p90=" << stats.p90 << " p99=" << stats.p99
            << " p99.9=" << stats.p999 << " p99.99=" << stats.p9999
            << " max=" << stats.max << '\n';
}

void print_sweep_stats(std::string_view name, int run,
                       std::size_t segment_count, std::size_t segment_gap,
                       const Stats& stats) {
  std::cout << "baseline name=" << name << " run=" << run
            << " segment_count=" << segment_count
            << " segment_gap=" << segment_gap
            << " iterations=" << stats.iterations
            << " latency_samples=" << stats.latency_samples
            << " mean_ns/op=" << stats.mean
            << " ops/s=" << stats.ops_per_second << " p50=" << stats.p50
            << " p90=" << stats.p90 << " p99=" << stats.p99
            << " p99.9=" << stats.p999 << " p99.99=" << stats.p9999
            << " max=" << stats.max << '\n';
}

std::string cpu_identifier() {
#if defined(_MSC_VER)
  char* value = nullptr;
  std::size_t value_size = 0;
  if (_dupenv_s(&value, &value_size, "PROCESSOR_IDENTIFIER") == 0 &&
      value != nullptr) {
    std::string result(value);
    std::free(value);
    return result;
  }
  return "unknown";
#else
  const char* const value = std::getenv("PROCESSOR_IDENTIFIER");
  return value == nullptr ? "unknown" : value;
#endif
}

template <typename Fn>
void run_benchmark(std::string_view name, Fn&& make_stats) {
  for (int run = 1; run <= benchmark_runs; ++run) {
    print_stats(name, run, make_stats());
  }
}

template <typename Fn>
void run_sweep_benchmark(std::string_view name, std::size_t segment_count,
                         std::size_t segment_gap, Fn&& make_stats) {
  for (int run = 1; run <= benchmark_runs; ++run) {
    print_sweep_stats(name, run, segment_count, segment_gap, make_stats());
  }
}

void print_build_context() {
#if defined(_MSC_VER)
  std::cout << "compiler=MSVC _MSC_VER=" << _MSC_VER
            << " _MSC_FULL_VER=" << _MSC_FULL_VER << '\n';
#else
  std::cout << "compiler=unknown\n";
#endif
#if defined(_M_X64)
  std::cout << "architecture=x64\n";
#elif defined(_M_IX86)
  std::cout << "architecture=x86\n";
#else
  std::cout << "architecture=unknown\n";
#endif
#if defined(NDEBUG)
  std::cout << "NDEBUG=defined\n";
  std::cout << "optimization=/O2\n";
#else
  std::cout << "NDEBUG=not_defined\n";
  std::cout << "optimization=/Od\n";
#endif
  std::cout << "cpu_identifier=" << cpu_identifier() << '\n';
  std::cout << "timer=std::chrono::steady_clock"
            << " runs=" << benchmark_runs
            << " steady_batches=" << steady_shape.batches
            << " steady_ops_per_batch=" << steady_shape.ops_per_batch << '\n';
  std::cout << "latency_sample_unit=batch_ns_per_op"
            << " throughput_pass=separate_unsampled_run\n";
  std::cout << "default_config min_price_tick="
            << default_config.min_price_tick
            << " max_price_tick=" << default_config.max_price_tick
            << " max_orders=" << default_config.max_orders << '\n';
}

void insert_checked(OrderBook& book, OrderId id, Side side, PriceTick price,
                    Quantity quantity = 100) {
  const InsertResult result =
      book.insert({id, id + 1000, side, price, quantity});
  if (!result.ok()) {
    std::abort();
  }
}

Stats bench_insert_existing_level() {
  struct State {
    OrderBook book;
    OrderId next_id;
  };
  return measure_scenario(
      steady_shape, [] { return State{OrderBook(default_config), 1}; },
      [](State& state, std::uint64_t, std::uint64_t) {
        insert_checked(state.book, state.next_id, Side::Ask, 100);
        g_sink += state.next_id;
        ++state.next_id;
      });
}

Stats bench_insert_new_level() {
  constexpr BatchShape shape{64, 64};
  struct State {
    OrderBook book;
    OrderId next_id;
  };
  return measure_scenario(
      shape, [] { return State{OrderBook({1, 4096, 4096}), 1}; },
      [](State& state, std::uint64_t, std::uint64_t) {
        const auto price = static_cast<PriceTick>(state.next_id);
        insert_checked(state.book, state.next_id, Side::Ask, price);
        g_sink += price;
        ++state.next_id;
      });
}

Stats bench_best(Side side) {
  struct State {
    OrderBook book;
  };
  return measure_scenario(
      steady_shape,
      [side] {
        State state{OrderBook(default_config)};
        for (OrderId id = 1; id <= 100000; ++id) {
          const PriceTick price =
              side == Side::Bid ? static_cast<PriceTick>(100 + (id % 64))
                                : static_cast<PriceTick>(200 + (id % 64));
          insert_checked(state.book, id, side, price);
        }
        return state;
      },
      [side](State& state, std::uint64_t, std::uint64_t) {
        const auto best = state.book.best(side);
        g_sink += best ? best->id : 0;
      });
}

Stats bench_set_remaining_hit() {
  struct State {
    OrderBook book;
    OrderId current_id;
  };
  return measure_scenario(
      steady_shape,
      [] {
        State state{OrderBook(default_config), 1};
        for (OrderId id = 1;
             id <= steady_shape.batches * steady_shape.ops_per_batch; ++id) {
          insert_checked(state.book, id, Side::Ask, 100, 1000);
        }
        return state;
      },
      [](State& state, std::uint64_t, std::uint64_t) {
        const SetRemainingResult result =
            state.book.set_remaining(state.current_id++, 500);
        g_sink += result.previous_remaining;
      });
}

Stats bench_set_remaining_miss() {
  struct State {
    OrderBook book;
    OrderId missing_id;
  };
  return measure_scenario(
      steady_shape,
      [] {
        State state{OrderBook(default_config), 10'000'000};
        for (OrderId id = 1; id <= 100000; ++id) {
          insert_checked(state.book, id, Side::Ask, 100);
        }
        return state;
      },
      [](State& state, std::uint64_t, std::uint64_t) {
        const SetRemainingResult result =
            state.book.set_remaining(state.missing_id++, 500);
        g_sink += static_cast<std::uint64_t>(result.status);
      });
}

Stats bench_erase_head() {
  struct State {
    OrderBook book;
    OrderId current_id;
  };
  return measure_scenario(
      steady_shape,
      [] {
        State state{OrderBook(default_config), 1};
        for (OrderId id = 1;
             id <= steady_shape.batches * steady_shape.ops_per_batch; ++id) {
          insert_checked(state.book, id, Side::Bid, 100);
        }
        return state;
      },
      [](State& state, std::uint64_t, std::uint64_t) {
        const EraseResult result = state.book.erase(state.current_id++);
        g_sink += result.removed.id;
      });
}

Stats bench_erase_middle() {
  constexpr OrderId group_count = 100000;
  struct State {
    OrderBook book;
    OrderId group;
  };
  return measure_scenario(
      {100, 1000},
      [] {
        State state{OrderBook(default_config), 0};
        for (OrderId group = 0; group < group_count; ++group) {
          const OrderId base = group * 3 + 1;
          insert_checked(state.book, base, Side::Bid, 100);
          insert_checked(state.book, base + 1, Side::Bid, 100);
          insert_checked(state.book, base + 2, Side::Bid, 100);
        }
        return state;
      },
      [](State& state, std::uint64_t, std::uint64_t) {
        const OrderId id = state.group * 3 + 2;
        const EraseResult result = state.book.erase(id);
        g_sink += result.removed.id;
        ++state.group;
      });
}

Stats bench_erase_tail() {
  constexpr OrderId group_count = 100000;
  struct State {
    OrderBook book;
    OrderId group;
  };
  return measure_scenario(
      {100, 1000},
      [] {
        State state{OrderBook(default_config), 0};
        for (OrderId group = 0; group < group_count; ++group) {
          const OrderId base = group * 3 + 1;
          insert_checked(state.book, base, Side::Bid, 100);
          insert_checked(state.book, base + 1, Side::Bid, 100);
          insert_checked(state.book, base + 2, Side::Bid, 100);
        }
        return state;
      },
      [](State& state, std::uint64_t, std::uint64_t) {
        const OrderId id = state.group * 3 + 3;
        const EraseResult result = state.book.erase(id);
        g_sink += result.removed.id;
        ++state.group;
      });
}

Stats bench_erase_only_at_level() {
  constexpr BatchShape shape{64, 64};
  struct State {
    OrderBook book;
    OrderId current_id;
  };
  return measure_scenario(
      shape,
      [] {
        State state{OrderBook({1, 4096, 4096}), 1};
        for (OrderId id = 1; id <= shape.batches * shape.ops_per_batch; ++id) {
          insert_checked(state.book, id, Side::Ask,
                         static_cast<PriceTick>(id));
        }
        return state;
      },
      [](State& state, std::uint64_t, std::uint64_t) {
        const EraseResult result = state.book.erase(state.current_id++);
        g_sink += result.removed.price;
      });
}

Stats bench_best_segment_recompute_near() {
  constexpr BatchShape shape{64, 1};
  struct State {
    OrderBook book;
  };
  return measure_scenario(
      shape,
      [] {
        State state{OrderBook({1, 4096, 128})};
        OrderId id = 1;
        for (PriceTick price = 64; price <= 4096; price += 64) {
          insert_checked(state.book, id++, Side::Ask, price);
        }
        return state;
      },
      [](State& state, std::uint64_t, std::uint64_t) {
        const auto best = state.book.best(Side::Ask);
        if (!best) {
          std::abort();
        }
        const EraseResult result = state.book.erase(best->id);
        g_sink += result.removed.id;
      });
}

Stats bench_best_segment_recompute_far() {
  constexpr BatchShape shape{64, 1};
  struct State {
    OrderBook book;
  };
  return measure_scenario(
      shape,
      [] {
        State state{OrderBook({1, 262144, 128})};
        OrderId id = 1;
        for (PriceTick price = 1; price <= 262144; price += 4096) {
          insert_checked(state.book, id++, Side::Ask, price);
        }
        return state;
      },
      [](State& state, std::uint64_t, std::uint64_t) {
        const auto best = state.book.best(Side::Ask);
        if (!best) {
          std::abort();
        }
        const EraseResult result = state.book.erase(best->id);
        g_sink += result.removed.id;
      });
}

Stats bench_best_segment_recompute_sweep(std::size_t segment_gap) {
  constexpr std::size_t segment_count = 4096;
  const PriceTick max_price =
      static_cast<PriceTick>((segment_count * prices_per_segment) - 1);
  const std::uint64_t occupied_segments =
      (segment_count + segment_gap - 1) / segment_gap;
  const BatchShape shape{occupied_segments, 1};
  struct State {
    OrderBook book;
  };
  return measure_scenario(
      shape,
      [max_price, segment_gap] {
        State state{OrderBook({0, max_price, 4096})};
        OrderId id = 1;
        for (std::size_t segment = 0; segment < segment_count;
             segment += segment_gap) {
          const PriceTick price =
              static_cast<PriceTick>(segment * prices_per_segment);
          insert_checked(state.book, id++, Side::Ask, price);
        }
        return state;
      },
      [](State& state, std::uint64_t, std::uint64_t) {
        const auto best = state.book.best(Side::Ask);
        if (!best) {
          std::abort();
        }
        const EraseResult result = state.book.erase(best->id);
        g_sink += result.removed.id;
      });
}

Stats bench_partial_fill_cycle() {
  const Quantity initial_quantity =
      static_cast<Quantity>(steady_shape.batches * steady_shape.ops_per_batch + 1);
  struct State {
    OrderBook book;
  };
  return measure_scenario(
      steady_shape,
      [initial_quantity] {
        State state{OrderBook(default_config)};
        for (OrderId id = 1;
             id <= steady_shape.batches * steady_shape.ops_per_batch; ++id) {
          insert_checked(state.book, id, Side::Ask, 100, initial_quantity);
        }
        return state;
      },
      [](State& state, std::uint64_t, std::uint64_t) {
        const auto best = state.book.best(Side::Ask);
        if (!best || best->remaining <= 1) {
          std::abort();
        }
        const SetRemainingResult result =
            state.book.set_remaining(best->id, best->remaining - 1);
        g_sink += result.previous_remaining;
      });
}

Stats bench_full_fill_cycle() {
  struct State {
    OrderBook book;
  };
  return measure_scenario(
      steady_shape,
      [] {
        State state{OrderBook(default_config)};
        for (OrderId id = 1;
             id <= steady_shape.batches * steady_shape.ops_per_batch; ++id) {
          insert_checked(state.book, id, Side::Ask, 100);
        }
        return state;
      },
      [](State& state, std::uint64_t, std::uint64_t) {
        const auto best = state.book.best(Side::Ask);
        if (!best) {
          std::abort();
        }
        const EraseResult result = state.book.erase(best->id);
        g_sink += result.removed.id;
      });
}

Stats bench_steady_state_churn_dense_fifo() {
  constexpr OrderId active_count = 100000;
  struct State {
    OrderBook book;
    std::vector<OrderId> active_ids;
    OrderId next_id;
    std::size_t cursor;
  };
  return measure_scenario(
      steady_shape,
      [] {
        State state{OrderBook(default_config), {}, active_count + 1, 0};
        state.active_ids.reserve(active_count);
        for (OrderId id = 1; id <= active_count; ++id) {
          insert_checked(state.book, id, Side::Ask, 100);
          state.active_ids.push_back(id);
        }
        return state;
      },
      [](State& state, std::uint64_t, std::uint64_t) {
        const OrderId old_id = state.active_ids[state.cursor];
        const EraseResult erased = state.book.erase(old_id);
        insert_checked(state.book, state.next_id, Side::Ask, 100);
        state.active_ids[state.cursor] = state.next_id;
        g_sink += erased.removed.id + state.next_id;
        ++state.next_id;
        state.cursor = (state.cursor + 1) % state.active_ids.size();
      });
}

Stats bench_steady_state_churn_mixed_occupancy_85() {
  constexpr OrderCapacity capacity = 100000;
  constexpr OrderId active_count = 85000;
  struct State {
    OrderBook book;
    std::vector<OrderId> active_ids;
    OrderId next_id;
    std::size_t cursor;
  };
  return measure_scenario(
      steady_shape,
      [] {
        State state{OrderBook({1, 4096, capacity}), {}, active_count + 1, 0};
        state.active_ids.reserve(active_count);
        for (OrderId id = 1; id <= active_count; ++id) {
          const Side side = (id & 1U) == 0 ? Side::Bid : Side::Ask;
          const PriceTick price =
              static_cast<PriceTick>(1 + ((id * 37) % 4096));
          insert_checked(state.book, id, side, price, 100);
          state.active_ids.push_back(id);
        }
        return state;
      },
      [](State& state, std::uint64_t, std::uint64_t) {
        const OrderId old_id = state.active_ids[state.cursor];
        const EraseResult erased = state.book.erase(old_id);
        const Side side = (state.next_id & 1U) == 0 ? Side::Bid : Side::Ask;
        const PriceTick price =
            static_cast<PriceTick>(1 + ((state.next_id * 37) % 4096));
        insert_checked(state.book, state.next_id, side, price, 100);
        state.active_ids[state.cursor] = state.next_id;
        g_sink += erased.removed.id + state.next_id;
        ++state.next_id;
        state.cursor = (state.cursor + 1) % state.active_ids.size();
      });
}

} // namespace

int main() {
  print_build_context();

  run_benchmark("construct_first_touch_100K", [] {
    return run_construction_first_touch(25, {1, 4096, 100000});
  });
  run_benchmark("insert_existing_level_dense_fifo",
                bench_insert_existing_level);
  run_benchmark("insert_new_level_many_levels", bench_insert_new_level);
  run_benchmark("best_bid_mixed_levels", [] { return bench_best(Side::Bid); });
  run_benchmark("best_ask_mixed_levels", [] { return bench_best(Side::Ask); });
  run_benchmark("set_remaining_hit", bench_set_remaining_hit);
  run_benchmark("set_remaining_miss", bench_set_remaining_miss);
  run_benchmark("erase_head_dense_fifo", bench_erase_head);
  run_benchmark("erase_middle_dense_fifo", bench_erase_middle);
  run_benchmark("erase_tail_dense_fifo", bench_erase_tail);
  run_benchmark("erase_only_at_level_many_levels", bench_erase_only_at_level);
  run_benchmark("best_segment_recompute_near",
                bench_best_segment_recompute_near);
  run_benchmark("best_segment_recompute_far",
                bench_best_segment_recompute_far);
  for (const std::size_t segment_gap : {1U, 2U, 4U, 8U, 16U, 32U, 64U}) {
    run_sweep_benchmark("best_segment_recompute_sweep", 4096, segment_gap,
                        [segment_gap] {
                          return bench_best_segment_recompute_sweep(
                              segment_gap);
                        });
  }
  run_benchmark("partial_fill_cycle_best_set",
                bench_partial_fill_cycle);
  run_benchmark("full_fill_cycle_best_erase", bench_full_fill_cycle);
  run_benchmark("steady_state_churn_dense_fifo",
                bench_steady_state_churn_dense_fifo);
  run_benchmark("steady_state_churn_mixed_occupancy_85",
                bench_steady_state_churn_mixed_occupancy_85);

  std::cout << "sink=" << g_sink << '\n';
  return 0;
}
