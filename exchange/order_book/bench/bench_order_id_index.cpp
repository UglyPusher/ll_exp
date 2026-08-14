/**
 * @file bench_order_id_index.cpp
 * @brief Reproducible fixed-index probe, repair, and churn benchmark.
 */
#include <fexma/order_book/order_id_index.hpp>

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

constexpr int benchmark_runs = 5;
constexpr std::size_t case_count = 16;
constexpr std::size_t latency_batches = 100;

struct IndexCase {
  explicit IndexCase(OrderCapacity capacity) : index(capacity) {}

  OrderIdIndex index;
  std::vector<OrderId> inserted_ids;
  OrderId target{};
  OrderIndex target_slot{};
};

struct Distribution {
  double mean{};
  std::size_t p50{};
  std::size_t p99{};
  std::size_t max{};
};

struct Stats {
  std::uint64_t iterations{};
  std::uint64_t latency_samples{};
  double mean_ns_per_op{};
  double operations_per_second{};
  double p50_ns_per_op{};
  double p99_ns_per_op{};
  double max_ns_per_op{};
  Distribution lookup;
  Distribution repair_scan;
  Distribution repair_moved;
};

[[nodiscard]] std::uint64_t mix(std::uint64_t value) noexcept {
  value ^= value >> 33;
  value *= 0xff51afd7ed558ccdULL;
  value ^= value >> 33;
  value *= 0xc4ceb9fe1a85ec53ULL;
  value ^= value >> 33;
  return value;
}

[[nodiscard]] std::uint64_t next_random(std::uint64_t& state) noexcept {
  state += 0x9e3779b97f4a7c15ULL;
  std::uint64_t value = state;
  value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
  value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
  return value ^ (value >> 31);
}

[[nodiscard]] std::vector<OrderId>
make_ids(std::size_t count, bool random_ids, std::uint64_t seed) {
  std::vector<OrderId> ids;
  ids.reserve(count);
  std::uint64_t state = seed;
  while (ids.size() < count) {
    const OrderId id = random_ids ? next_random(state)
                                  : static_cast<OrderId>(ids.size() + seed);
    if (id != 0) {
      ids.push_back(id);
    }
  }
  return ids;
}

[[nodiscard]] std::vector<OrderId>
make_colliding_ids(std::size_t count, std::size_t bucket_count,
                   std::size_t home_bucket, OrderId first_id = 1) {
  std::vector<OrderId> ids;
  ids.reserve(count);
  for (OrderId id = first_id; ids.size() < count; ++id) {
    if ((mix(id) & (bucket_count - 1U)) == home_bucket) {
      ids.push_back(id);
    }
  }
  return ids;
}

void rebuild(IndexCase& state) {
  state.index.clear();
  for (std::size_t i = 0; i < state.inserted_ids.size(); ++i) {
    const IndexInsertStatus status = state.index.insert(
        state.inserted_ids[i], static_cast<OrderIndex>(i + 1U));
    if (status != IndexInsertStatus::Ok) {
      std::abort();
    }
  }
}

[[nodiscard]] Distribution summarize(std::vector<std::size_t> values) {
  if (values.empty()) {
    return {};
  }
  std::uint64_t sum = 0;
  for (const std::size_t value : values) {
    sum += value;
  }
  std::sort(values.begin(), values.end());
  const auto percentile = [&values](double percentile_value) {
    const std::size_t index = static_cast<std::size_t>(
        (static_cast<double>(values.size() - 1U) * percentile_value) / 100.0);
    return values[index];
  };
  return {static_cast<double>(sum) / static_cast<double>(values.size()),
          percentile(50.0), percentile(99.0), values.back()};
}

template <typename SetupFn, typename OperationFn, typename RestoreFn>
Stats measure_reversible(SetupFn&& setup, OperationFn&& operation,
                         RestoreFn&& restore) {
  std::vector<double> latency_samples;
  latency_samples.reserve(latency_batches);
  double latency_total_ns = 0.0;
  std::uint64_t iterations = 0;

  {
    std::vector<IndexCase> states = setup();
    for (std::size_t batch = 0; batch < latency_batches; ++batch) {
      const auto start = std::chrono::steady_clock::now();
      for (IndexCase& state : states) {
        operation(state, nullptr);
      }
      const auto stop = std::chrono::steady_clock::now();
      const double elapsed = static_cast<double>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(stop - start)
              .count());
      latency_total_ns += elapsed;
      iterations += states.size();
      latency_samples.push_back(elapsed / static_cast<double>(states.size()));
      for (IndexCase& state : states) {
        restore(state);
      }
    }
  }

  double throughput_total_ns = 0.0;
  {
    std::vector<IndexCase> states = setup();
    for (std::size_t batch = 0; batch < latency_batches; ++batch) {
      const auto start = std::chrono::steady_clock::now();
      for (IndexCase& state : states) {
        operation(state, nullptr);
      }
      const auto stop = std::chrono::steady_clock::now();
      throughput_total_ns += static_cast<double>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(stop - start)
              .count());
      for (IndexCase& state : states) {
        restore(state);
      }
    }
  }

  std::vector<std::size_t> lookup;
  std::vector<std::size_t> repair_scan;
  std::vector<std::size_t> repair_moved;
  lookup.reserve(iterations);
  repair_scan.reserve(iterations);
  repair_moved.reserve(iterations);
  {
    std::vector<IndexCase> states = setup();
    for (std::size_t batch = 0; batch < latency_batches; ++batch) {
      for (IndexCase& state : states) {
        IndexProbeStats probe_stats{};
        operation(state, &probe_stats);
        lookup.push_back(probe_stats.lookup_probes);
        repair_scan.push_back(probe_stats.repair_buckets_scanned);
        repair_moved.push_back(probe_stats.repair_buckets_moved);
      }
      for (IndexCase& state : states) {
        restore(state);
      }
    }
  }

  std::sort(latency_samples.begin(), latency_samples.end());
  const auto latency_percentile = [&latency_samples](double value) {
    const std::size_t index = static_cast<std::size_t>(
        (static_cast<double>(latency_samples.size() - 1U) * value) / 100.0);
    return latency_samples[index];
  };
  return {iterations,
          latency_samples.size(),
          latency_total_ns / static_cast<double>(iterations),
          (static_cast<double>(iterations) * 1'000'000'000.0) /
              throughput_total_ns,
          latency_percentile(50.0),
          latency_percentile(99.0),
          latency_samples.back(),
          summarize(std::move(lookup)),
          summarize(std::move(repair_scan)),
          summarize(std::move(repair_moved))};
}

[[nodiscard]] std::vector<IndexCase>
make_occupancy_cases(int occupancy_percent, bool random_ids,
                     bool include_target) {
  constexpr OrderCapacity capacity = 256;
  OrderIdIndex sizing_index(capacity);
  const std::size_t occupied =
      sizing_index.bucket_count() * static_cast<std::size_t>(occupancy_percent) /
      100U;
  const std::vector<OrderId> ids =
      make_ids(occupied + 1U, random_ids, random_ids ? 0xC0FFEEU : 1U);

  std::vector<IndexCase> states;
  states.reserve(case_count);
  for (std::size_t case_index = 0; case_index < case_count; ++case_index) {
    states.emplace_back(capacity);
    IndexCase& state = states.back();
    state.inserted_ids.assign(ids.begin(), ids.begin() + occupied);
    state.target = include_target ? state.inserted_ids[occupied / 2U]
                                  : ids.back();
    state.target_slot = static_cast<OrderIndex>(occupied + 1U);
    rebuild(state);
  }
  return states;
}

[[nodiscard]] std::vector<IndexCase>
make_cluster_cases(std::size_t cluster_length, bool wrap_around) {
  constexpr OrderCapacity capacity = 256;
  OrderIdIndex sizing_index(capacity);
  const std::size_t home = wrap_around ? sizing_index.bucket_count() - 1U : 17U;
  const std::vector<OrderId> ids = make_colliding_ids(
      cluster_length, sizing_index.bucket_count(), home);

  std::vector<IndexCase> states;
  states.reserve(case_count);
  for (std::size_t case_index = 0; case_index < case_count; ++case_index) {
    states.emplace_back(capacity);
    IndexCase& state = states.back();
    state.inserted_ids = ids;
    state.target = ids.front();
    state.target_slot = 1;
    rebuild(state);
  }
  return states;
}

template <typename SetupFn, typename OperationFn, typename RestoreFn>
void run(std::string_view name, SetupFn&& setup, OperationFn&& operation,
         RestoreFn&& restore) {
  for (int benchmark_run = 1; benchmark_run <= benchmark_runs;
       ++benchmark_run) {
    const Stats stats = measure_reversible(setup, operation, restore);
    std::cout << "index name=" << name << " run=" << benchmark_run
              << " iterations=" << stats.iterations
              << " latency_samples=" << stats.latency_samples
              << " mean_ns/op=" << stats.mean_ns_per_op
              << " ops/s=" << stats.operations_per_second
              << " p50=" << stats.p50_ns_per_op
              << " p99=" << stats.p99_ns_per_op
              << " max=" << stats.max_ns_per_op
              << " lookup_mean=" << stats.lookup.mean
              << " lookup_p50=" << stats.lookup.p50
              << " lookup_p99=" << stats.lookup.p99
              << " lookup_max=" << stats.lookup.max
              << " repair_scan_mean=" << stats.repair_scan.mean
              << " repair_scan_p99=" << stats.repair_scan.p99
              << " repair_scan_max=" << stats.repair_scan.max
              << " repair_moved_mean=" << stats.repair_moved.mean
              << " repair_moved_p99=" << stats.repair_moved.p99
              << " repair_moved_max=" << stats.repair_moved.max << '\n';
  }
}

void run_occupancy_scenarios(int occupancy_percent, bool random_ids) {
  const std::string suffix = std::to_string(occupancy_percent) +
                             (random_ids ? "_random" : "_sequential");
  const auto hit_setup = [=] {
    return make_occupancy_cases(occupancy_percent, random_ids, true);
  };
  const auto miss_setup = [=] {
    return make_occupancy_cases(occupancy_percent, random_ids, false);
  };

  run("find_hit_" + suffix, hit_setup,
      [](IndexCase& state, IndexProbeStats* stats) {
        g_sink += state.index.find(state.target, stats);
      },
      [](IndexCase&) {});
  run("find_miss_" + suffix, miss_setup,
      [](IndexCase& state, IndexProbeStats* stats) {
        g_sink += state.index.find(state.target, stats);
      },
      [](IndexCase&) {});
  run("insert_success_" + suffix, miss_setup,
      [](IndexCase& state, IndexProbeStats* stats) {
        g_sink += static_cast<std::uint8_t>(
            state.index.insert(state.target, state.target_slot, stats));
      },
      [](IndexCase& state) { (void)state.index.erase(state.target); });
  run("insert_duplicate_" + suffix, hit_setup,
      [](IndexCase& state, IndexProbeStats* stats) {
        g_sink += static_cast<std::uint8_t>(
            state.index.insert(state.target, state.target_slot, stats));
      },
      [](IndexCase&) {});
  run("erase_hit_" + suffix, hit_setup,
      [](IndexCase& state, IndexProbeStats* stats) {
        g_sink += state.index.erase(state.target, stats) ? 1U : 0U;
      },
      [](IndexCase& state) { rebuild(state); });
  run("erase_miss_" + suffix, miss_setup,
      [](IndexCase& state, IndexProbeStats* stats) {
        g_sink += state.index.erase(state.target, stats) ? 1U : 0U;
      },
      [](IndexCase&) {});
}

void run_cluster_scenario(std::size_t cluster_length, bool wrap_around) {
  const std::string name = "erase_cluster_" + std::to_string(cluster_length) +
                           (wrap_around ? "_wrap" : "_linear");
  const auto setup = [=] { return make_cluster_cases(cluster_length, wrap_around); };
  run(name, setup,
      [](IndexCase& state, IndexProbeStats* stats) {
        g_sink += state.index.erase(state.target, stats) ? 1U : 0U;
      },
      [](IndexCase& state) { rebuild(state); });
}

struct ChurnState {
  explicit ChurnState(OrderCapacity capacity) : index(capacity) {}
  OrderIdIndex index;
  std::vector<OrderId> active;
  std::size_t victim{};
};

Stats measure_long_churn(const std::vector<OrderId>& replacements,
                         bool random_initial_ids) {
  constexpr OrderCapacity capacity = 256;
  constexpr std::size_t active_count = 256;
  constexpr std::size_t cycles_per_batch = 1000;
  const std::size_t batches = replacements.size() / cycles_per_batch;

  const auto setup = [&] {
    ChurnState state(capacity);
    state.active = make_ids(active_count, random_initial_ids, 0x12345678U);
    for (std::size_t i = 0; i < state.active.size(); ++i) {
      if (state.index.insert(state.active[i], static_cast<OrderIndex>(i)) !=
          IndexInsertStatus::Ok) {
        std::abort();
      }
    }
    return state;
  };

  std::vector<double> samples;
  samples.reserve(batches);
  double latency_ns = 0.0;
  {
    ChurnState state = setup();
    std::size_t replacement = 0;
    for (std::size_t batch = 0; batch < batches; ++batch) {
      const auto start = std::chrono::steady_clock::now();
      for (std::size_t cycle = 0; cycle < cycles_per_batch; ++cycle) {
        const std::size_t victim = state.victim++ % state.active.size();
        g_sink += state.index.erase(state.active[victim]) ? 1U : 0U;
        const OrderId next = replacements[replacement++];
        g_sink += static_cast<std::uint8_t>(state.index.insert(
            next, static_cast<OrderIndex>(victim)));
        state.active[victim] = next;
      }
      const auto stop = std::chrono::steady_clock::now();
      const double elapsed = static_cast<double>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(stop - start)
              .count());
      latency_ns += elapsed;
      samples.push_back(elapsed /
                        static_cast<double>(cycles_per_batch * 2U));
    }
  }

  double throughput_ns = 0.0;
  {
    ChurnState state = setup();
    std::size_t replacement = 0;
    const auto start = std::chrono::steady_clock::now();
    for (std::size_t batch = 0; batch < batches; ++batch) {
      for (std::size_t cycle = 0; cycle < cycles_per_batch; ++cycle) {
        const std::size_t victim = state.victim++ % state.active.size();
        (void)state.index.erase(state.active[victim]);
        const OrderId next = replacements[replacement++];
        (void)state.index.insert(next, static_cast<OrderIndex>(victim));
        state.active[victim] = next;
      }
    }
    const auto stop = std::chrono::steady_clock::now();
    throughput_ns = static_cast<double>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(stop - start)
            .count());
  }

  std::vector<std::size_t> lookup;
  std::vector<std::size_t> repair_scan;
  std::vector<std::size_t> repair_moved;
  {
    ChurnState state = setup();
    const std::size_t diagnostic_cycles =
        (std::min)(std::size_t{100000}, replacements.size());
    for (std::size_t replacement = 0; replacement < diagnostic_cycles;
         ++replacement) {
      const std::size_t victim = state.victim++ % state.active.size();
      IndexProbeStats erase_stats{};
      (void)state.index.erase(state.active[victim], &erase_stats);
      lookup.push_back(erase_stats.lookup_probes);
      repair_scan.push_back(erase_stats.repair_buckets_scanned);
      repair_moved.push_back(erase_stats.repair_buckets_moved);
      const OrderId next = replacements[replacement];
      IndexProbeStats insert_stats{};
      (void)state.index.insert(next, static_cast<OrderIndex>(victim),
                               &insert_stats);
      lookup.push_back(insert_stats.lookup_probes);
      repair_scan.push_back(0);
      repair_moved.push_back(0);
      state.active[victim] = next;
    }
  }

  std::sort(samples.begin(), samples.end());
  const std::uint64_t operations = replacements.size() * 2U;
  const auto percentile = [&samples](double value) {
    const std::size_t index = static_cast<std::size_t>(
        (static_cast<double>(samples.size() - 1U) * value) / 100.0);
    return samples[index];
  };
  return {operations,
          samples.size(),
          latency_ns / static_cast<double>(operations),
          (static_cast<double>(operations) * 1'000'000'000.0) / throughput_ns,
          percentile(50.0),
          percentile(99.0),
          samples.back(),
          summarize(std::move(lookup)),
          summarize(std::move(repair_scan)),
          summarize(std::move(repair_moved))};
}

void run_long_churn() {
  constexpr std::size_t cycles = 5'000'000;
  const std::vector<OrderId> replacements =
      make_ids(cycles, true, 0xA11CE5EEDULL);
  for (int benchmark_run = 1; benchmark_run <= benchmark_runs;
       ++benchmark_run) {
    const Stats stats = measure_long_churn(replacements, true);
    std::cout << "index name=churn_random_10M run=" << benchmark_run
              << " iterations=" << stats.iterations
              << " latency_samples=" << stats.latency_samples
              << " mean_ns/op=" << stats.mean_ns_per_op
              << " ops/s=" << stats.operations_per_second
              << " p50=" << stats.p50_ns_per_op
              << " p99=" << stats.p99_ns_per_op
              << " max=" << stats.max_ns_per_op
              << " lookup_mean=" << stats.lookup.mean
              << " lookup_p99=" << stats.lookup.p99
              << " lookup_max=" << stats.lookup.max
              << " repair_scan_mean=" << stats.repair_scan.mean
              << " repair_scan_p99=" << stats.repair_scan.p99
              << " repair_scan_max=" << stats.repair_scan.max
              << " repair_moved_mean=" << stats.repair_moved.mean
              << " repair_moved_p99=" << stats.repair_moved.p99
              << " repair_moved_max=" << stats.repair_moved.max << '\n';
  }
}

void print_context() {
#if defined(_MSC_VER)
  std::cout << "compiler=MSVC _MSC_VER=" << _MSC_VER
            << " _MSC_FULL_VER=" << _MSC_FULL_VER << '\n';
#endif
#if defined(NDEBUG)
  std::cout << "NDEBUG=defined optimization=/O2\n";
#else
  std::cout << "NDEBUG=not_defined\n";
#endif
  std::cout << "runs=" << benchmark_runs
            << " case_count=" << case_count
            << " latency_batches=" << latency_batches
            << " latency_sample_unit=batch_ns_per_op"
            << " throughput_pass=separate_unsampled_batches\n";
  std::cout << "layout sizeof(OrderIdIndex)=" << sizeof(OrderIdIndex)
            << " bucket_size=" << OrderIdIndex::bucket_size()
            << " bucket_align=" << OrderIdIndex::bucket_align() << '\n';
}

} // namespace

int main() {
  print_context();
  for (const int occupancy : {10, 25, 50}) {
    run_occupancy_scenarios(occupancy, false);
    run_occupancy_scenarios(occupancy, true);
  }
  for (const std::size_t cluster_length : {std::size_t{1}, std::size_t{2},
                                           std::size_t{4}, std::size_t{8},
                                           std::size_t{16}, std::size_t{32},
                                           std::size_t{64}, std::size_t{128}}) {
    run_cluster_scenario(cluster_length, false);
    run_cluster_scenario(cluster_length, true);
  }
  run_long_churn();
  std::cout << "sink=" << g_sink << '\n';
  return 0;
}
