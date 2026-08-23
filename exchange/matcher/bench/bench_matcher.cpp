/**
 * @file bench_matcher.cpp
 * @brief Stabilized matcher cost-decomposition benchmark harness.
 */
#include <fexma/matcher/matcher.hpp>
#include <fexma/order_book/order_book.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#endif

using namespace fexma::matcher;

namespace {

inline constexpr ClientId benchmark_client_id = 17;

volatile std::uint64_t g_sink = 0;

struct Stats {
  std::uint64_t iterations{};
  std::uint64_t latency_samples{};
  double mean{};
  double commands_per_second{};
  double p50{};
  double p90{};
  double p99{};
  double p999{};
  double max{};
};

struct Summary {
  std::string name;
  std::vector<Stats> runs;
};

struct BatchShape {
  std::uint64_t batches;
  std::uint64_t commands_per_batch;
};

struct Config {
  BatchShape shape{300, 1000};
  std::uint64_t warmup_commands{100000};
  int runs{5};
  int requested_cpu{-1};
};

constexpr OrderBookConfig default_config{1, 4096, 400000};
constexpr OrderBookConfig run_batch_config{1, 4096, 2048};

class ShutdownCommandReader {
public:
  [[nodiscard]] CommandReadResult read_next() noexcept {
    return {CommandReadStatus::Ok,
            {1, {benchmark_client_id, Command{ShutdownCommand{}}}}};
  }
};

class ArrayCommandReader {
public:
  explicit ArrayCommandReader(const std::vector<CommandEnvelope>& commands)
      : commands_(commands) {}

  [[nodiscard]] CommandReadResult read_next() noexcept {
    if (next_ == commands_.size()) {
      return {CommandReadStatus::Ok,
              {commands_.empty() ? 1
                                 : commands_.back().command_sequence + 1,
               {benchmark_client_id, Command{ShutdownCommand{}}}}};
    }
    return {CommandReadStatus::Ok, commands_[next_++]};
  }

private:
  const std::vector<CommandEnvelope>& commands_;
  std::size_t next_{};
};

class NullEventWriter {
public:
  [[nodiscard]] PublishResult publish(const EventEnvelope&) noexcept {
    return {PublishStatus::Ok};
  }
};

class CountingEventWriter {
public:
  [[nodiscard]] PublishResult publish(const EventEnvelope& envelope) noexcept {
    const Event& event = envelope.payload.message;
    switch (event.type) {
    case EventType::None:
      break;
    case EventType::OrderAccepted:
      ++accepted;
      break;
    case EventType::OrderRejected:
      ++rejected;
      break;
    case EventType::Trade:
      ++trades;
      break;
    case EventType::OrderRested:
      ++rested;
      break;
    case EventType::OrderDone:
      ++done;
      break;
    case EventType::SaveSnapshot:
    case EventType::LoadSnapshot:
    case EventType::Shutdown:
      break;
    case EventType::MatcherFatal:
      break;
    }
    return {PublishStatus::Ok};
  }

  std::uint64_t accepted{};
  std::uint64_t rejected{};
  std::uint64_t trades{};
  std::uint64_t rested{};
  std::uint64_t done{};
};

class SinkEventWriter {
public:
  [[nodiscard]] PublishResult publish(const EventEnvelope& envelope) noexcept {
    const Event& event = envelope.payload.message;
    switch (event.type) {
    case EventType::None:
      break;
    case EventType::OrderAccepted:
      ++accepted;
      g_sink += event.accepted.id;
      break;
    case EventType::OrderRejected:
      ++rejected;
      g_sink += event.rejected.id;
      break;
    case EventType::Trade:
      ++trades;
      g_sink += event.trade.maker_order_id + event.trade.quantity;
      break;
    case EventType::OrderRested:
      ++rested;
      g_sink += event.rested.id + event.rested.remaining;
      break;
    case EventType::OrderDone:
      ++done;
      g_sink += event.done.id;
      break;
    case EventType::SaveSnapshot:
      g_sink += envelope.payload.caused_by_command_sequence;
      break;
    case EventType::LoadSnapshot:
      g_sink += event.load_snapshot.save_snapshot_command_sequence +
                envelope.payload.caused_by_command_sequence +
                event.load_snapshot.snapshot_epoch_id;
      break;
    case EventType::Shutdown:
      g_sink += envelope.event_sequence;
      break;
    case EventType::MatcherFatal:
      g_sink += event.fatal.offending_order_id + event.fatal.last_order_id +
                static_cast<std::uint64_t>(event.fatal.reason);
      break;
    }
    return {PublishStatus::Ok};
  }

  std::uint64_t accepted{};
  std::uint64_t rejected{};
  std::uint64_t trades{};
  std::uint64_t rested{};
  std::uint64_t done{};
};

[[nodiscard]] Command new_limit(OrderId id, OwnerId owner_id, Side side,
                                PriceTick price, Quantity quantity) noexcept {
  return Command{NewLimitOrder{id, owner_id, side, price, quantity}};
}

[[nodiscard]] double median(std::vector<double> values) {
  std::sort(values.begin(), values.end());
  return values[values.size() / 2U];
}

[[nodiscard]] double percentile(const std::vector<double>& samples, double p) {
  const std::size_t index = static_cast<std::size_t>(
      (static_cast<double>(samples.size() - 1U) * p) / 100.0);
  return samples[index];
}

template <typename SetupFn, typename OperationFn>
void warmup(Config config, SetupFn&& setup, OperationFn&& operation) {
  auto state = setup();
  for (std::uint64_t command = 0; command < config.warmup_commands;
       ++command) {
    operation(*state, command / config.shape.commands_per_batch,
              command % config.shape.commands_per_batch);
  }
}

template <typename SetupFn, typename OperationFn, typename ConsumeFn>
Stats measure_scenario(Config config, SetupFn&& setup, OperationFn&& operation,
                       ConsumeFn&& consume) {
  warmup(config, setup, operation);

  std::vector<double> samples;
  samples.reserve(static_cast<std::size_t>(config.shape.batches));
  double total_ns = 0.0;

  {
    auto state = setup();
    for (std::uint64_t batch = 0; batch < config.shape.batches; ++batch) {
      const auto start = std::chrono::steady_clock::now();
      for (std::uint64_t command = 0;
           command < config.shape.commands_per_batch; ++command) {
        operation(*state, batch, command);
      }
      const auto stop = std::chrono::steady_clock::now();
      const double elapsed = static_cast<double>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(stop - start)
              .count());
      total_ns += elapsed;
      samples.push_back(elapsed /
                        static_cast<double>(config.shape.commands_per_batch));
    }
    consume(*state);
  }

  const std::uint64_t iterations =
      config.shape.batches * config.shape.commands_per_batch;
  double throughput_ns = 0.0;
  {
    auto state = setup();
    const auto start = std::chrono::steady_clock::now();
    for (std::uint64_t batch = 0; batch < config.shape.batches; ++batch) {
      for (std::uint64_t command = 0;
           command < config.shape.commands_per_batch; ++command) {
        operation(*state, batch, command);
      }
    }
    const auto stop = std::chrono::steady_clock::now();
    throughput_ns = static_cast<double>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(stop - start)
            .count());
    consume(*state);
  }

  std::sort(samples.begin(), samples.end());
  return {iterations,
          config.shape.batches,
          total_ns / static_cast<double>(iterations),
          (static_cast<double>(iterations) * 1'000'000'000.0) / throughput_ns,
          percentile(samples, 50.0),
          percentile(samples, 90.0),
          percentile(samples, 99.0),
          percentile(samples, 99.9),
          samples.back()};
}

void print_stats(std::string_view name, int run, const Stats& stats) {
  std::cout << "matcher name=" << name << " run=" << run
            << " iterations=" << stats.iterations
            << " latency_samples=" << stats.latency_samples
            << " mean_ns/command=" << stats.mean
            << " commands/s=" << stats.commands_per_second
            << " p50=" << stats.p50 << " p90=" << stats.p90
            << " p99=" << stats.p99 << " p99.9=" << stats.p999
            << " max=" << stats.max << '\n';
}

void print_summary(const std::vector<Summary>& summaries) {
  std::cout << "summary scenario median_mean min_run_mean max_run_mean"
            << " median_p50 median_p90 median_p99 median_p99.9\n";
  for (const Summary& summary : summaries) {
    std::vector<double> means;
    std::vector<double> p50s;
    std::vector<double> p90s;
    std::vector<double> p99s;
    std::vector<double> p999s;
    means.reserve(summary.runs.size());
    p50s.reserve(summary.runs.size());
    p90s.reserve(summary.runs.size());
    p99s.reserve(summary.runs.size());
    p999s.reserve(summary.runs.size());
    for (const Stats& stats : summary.runs) {
      means.push_back(stats.mean);
      p50s.push_back(stats.p50);
      p90s.push_back(stats.p90);
      p99s.push_back(stats.p99);
      p999s.push_back(stats.p999);
    }
    const auto minmax = std::minmax_element(means.begin(), means.end());
    std::cout << "summary scenario=" << summary.name
              << " median_mean=" << median(means)
              << " min_run_mean=" << *minmax.first
              << " max_run_mean=" << *minmax.second
              << " median_p50=" << median(p50s)
              << " median_p90=" << median(p90s)
              << " median_p99=" << median(p99s)
              << " median_p99.9=" << median(p999s) << '\n';
  }
}

#ifdef _WIN32
void configure_windows_runtime(const Config& config) {
  bool pinned = false;
  DWORD_PTR requested_mask = 0;
  DWORD_PTR previous_mask = 0;
  if (config.requested_cpu >= 0) {
    if (config.requested_cpu < static_cast<int>(sizeof(DWORD_PTR) * 8U)) {
      requested_mask = DWORD_PTR{1} << config.requested_cpu;
      previous_mask = SetThreadAffinityMask(GetCurrentThread(), requested_mask);
      pinned = previous_mask != 0;
    }
  }

  if (!SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS)) {
    std::cout << "warning=SetPriorityClass_failed error="
              << GetLastError() << '\n';
  }
  if (!SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST)) {
    std::cout << "warning=SetThreadPriority_failed error="
              << GetLastError() << '\n';
  }

  LARGE_INTEGER frequency{};
  (void)QueryPerformanceFrequency(&frequency);
  DWORD_PTR process_affinity = 0;
  DWORD_PTR system_affinity = 0;
  (void)GetProcessAffinityMask(GetCurrentProcess(), &process_affinity,
                               &system_affinity);
  const DWORD_PTR effective_thread_affinity = pinned ? requested_mask
                                                     : process_affinity;
  std::cout << "requested_cpu=" << config.requested_cpu
            << " pinning_success=" << (pinned ? "true" : "false")
            << " requested_affinity=0x" << std::hex << requested_mask
            << " previous_thread_affinity=0x" << previous_mask
            << " effective_affinity=0x" << effective_thread_affinity
            << " process_affinity=0x" << process_affinity
            << " system_affinity=0x" << system_affinity << std::dec
            << " current_processor=" << GetCurrentProcessorNumber()
            << " process_priority=" << GetPriorityClass(GetCurrentProcess())
            << " thread_priority=" << GetThreadPriority(GetCurrentThread())
            << " qpc_frequency=" << frequency.QuadPart
            << " smt_sibling=unknown\n";
  if (config.requested_cpu >= static_cast<int>(sizeof(DWORD_PTR) * 8U)) {
    std::cout << "warning=requested_cpu_requires_processor_group_support\n";
  }
}
#else
void configure_windows_runtime(const Config& config) {
  std::cout << "requested_cpu=" << config.requested_cpu
            << " pinning_success=false effective_affinity=unavailable"
            << " current_processor=unavailable process_priority=unavailable"
            << " thread_priority=unavailable smt_sibling=unknown\n";
}
#endif

void print_build_context(const Config& config) {
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
  std::cout << "NDEBUG=defined optimization=/O2\n";
#else
  std::cout << "NDEBUG=not_defined optimization=/Od\n";
#endif
  std::cout << "timer=std::chrono::steady_clock"
            << " timer_mode=steady"
            << " warmup_commands=" << config.warmup_commands
            << " runs=" << config.runs
            << " batches=" << config.shape.batches
            << " commands_per_batch=" << config.shape.commands_per_batch
            << '\n';
  std::cout << "latency_sample_unit=batch_ns_per_command"
            << " throughput_pass=separate_unsampled_run"
            << " interleaving=round_robin_by_run\n";
  std::cout << "default_config min_price_tick="
            << default_config.min_price_tick
            << " max_price_tick=" << default_config.max_price_tick
            << " max_orders=" << default_config.max_orders << '\n';
  std::cout << "run_batch_config max_orders="
            << run_batch_config.max_orders
            << " note=run_latency_uses_independent_batch_states\n";
  std::cout << "dce_guard=state_consumed_after_timed_region"
            << " null_writer_no_sink_writes=true"
            << " sink_writer_has_volatile_publish_writes=true"
            << " tsc_timer=todo\n";
}

struct BookState {
  explicit BookState(const OrderBookConfig& config) : book(config) {}

  fexma::order_book::OrderBook book;
  OrderId next_order_id{1};
};

void consume_book_state(const BookState& state) {
  const auto best_bid = state.book.best(Side::Bid);
  const auto best_ask = state.book.best(Side::Ask);
  g_sink += best_bid ? best_bid->id + best_bid->remaining : 1;
  g_sink += best_ask ? best_ask->id + best_ask->remaining : 1;
  g_sink += state.next_order_id;
}

void insert_checked(fexma::order_book::OrderBook& book, OrderId id,
                    OwnerId owner_id, Side side, PriceTick price,
                    Quantity quantity) {
  const fexma::order_book::InsertResult result =
      book.insert({id, owner_id, side, price, quantity});
  if (!result.ok()) {
    std::abort();
  }
}

void preload_book_asks(BookState& state, std::uint64_t count,
                       PriceTick price, Quantity quantity) {
  for (std::uint64_t i = 0; i < count; ++i) {
    const OrderId id = state.next_order_id++;
    insert_checked(state.book, id, id + 1000, Side::Ask, price, quantity);
  }
}

Stats bench_order_book_non_crossing(Config config) {
  return measure_scenario(
      config, [] { return std::make_unique<BookState>(default_config); },
      [](BookState& state, std::uint64_t, std::uint64_t) {
        const OrderId id = state.next_order_id++;
        insert_checked(state.book, id, id + 1000, Side::Bid, 100, 1);
      },
      consume_book_state);
}

Stats bench_order_book_full_fill(Config config) {
  return measure_scenario(
      config,
      [config] {
        auto state = std::make_unique<BookState>(default_config);
        preload_book_asks(*state,
                          config.shape.batches *
                              config.shape.commands_per_batch,
                          100, 1);
        return state;
      },
      [](BookState& state, std::uint64_t, std::uint64_t) {
        const auto best = state.book.best(Side::Ask);
        if (!best) {
          std::abort();
        }
        const fexma::order_book::EraseResult erased =
            state.book.erase(best->id);
        if (!erased.ok()) {
          std::abort();
        }
        ++state.next_order_id;
      },
      consume_book_state);
}

Stats bench_order_book_partial_fill(Config config) {
  return measure_scenario(
      config,
      [config] {
        auto state = std::make_unique<BookState>(default_config);
        preload_book_asks(*state, 1, 100,
                          static_cast<Quantity>(
                              config.shape.batches *
                                  config.shape.commands_per_batch +
                              config.warmup_commands + 1));
        return state;
      },
      [](BookState& state, std::uint64_t, std::uint64_t) {
        const auto best = state.book.best(Side::Ask);
        if (!best || best->remaining <= 1) {
          std::abort();
        }
        const fexma::order_book::SetRemainingResult changed =
            state.book.set_remaining(best->id, best->remaining - 1);
        if (!changed.ok()) {
          std::abort();
        }
        ++state.next_order_id;
      },
      consume_book_state);
}

template <typename Writer>
struct ProcessState {
  explicit ProcessState(const OrderBookConfig& config)
      : matcher(reader, writer, config) {}

  ShutdownCommandReader reader;
  Writer writer;
  Matcher<ShutdownCommandReader, Writer> matcher;
  OrderId next_order_id{1};
  CommandSequence next_command_sequence{1};
};

template <typename Writer>
void consume_process_state(const ProcessState<Writer>& state) {
  const auto best_bid = state.matcher.book().best(Side::Bid);
  const auto best_ask = state.matcher.book().best(Side::Ask);
  g_sink += best_bid ? best_bid->id + best_bid->remaining : 1;
  g_sink += best_ask ? best_ask->id + best_ask->remaining : 1;
  g_sink += state.next_order_id;
}

void consume_process_state(const ProcessState<CountingEventWriter>& state) {
  consume_process_state<CountingEventWriter>(state);
  g_sink += state.writer.accepted + state.writer.rejected +
            state.writer.trades + state.writer.rested + state.writer.done;
}

void consume_process_state(const ProcessState<SinkEventWriter>& state) {
  consume_process_state<SinkEventWriter>(state);
  g_sink += state.writer.accepted + state.writer.rejected +
            state.writer.trades + state.writer.rested + state.writer.done;
}

template <typename Writer>
void process_checked(ProcessState<Writer>& state, const Command& command) {
  const CommandEnvelope envelope{
      state.next_command_sequence++, {benchmark_client_id, command}};
  const ProcessResult result = state.matcher.process(envelope);
  if (result.status != ProcessStatus::Continue) {
    std::abort();
  }
}

template <typename Writer>
void preload_matcher_asks(ProcessState<Writer>& state, std::uint64_t count,
                          PriceTick price, Quantity quantity) {
  for (std::uint64_t i = 0; i < count; ++i) {
    const OrderId id = state.next_order_id++;
    process_checked(state,
                    new_limit(id, id + 1000, Side::Ask, price, quantity));
  }
}

template <typename Writer>
Stats bench_matcher_non_crossing(Config config) {
  return measure_scenario(
      config,
      [] { return std::make_unique<ProcessState<Writer>>(default_config); },
      [](ProcessState<Writer>& state, std::uint64_t, std::uint64_t) {
        const OrderId id = state.next_order_id++;
        process_checked(state,
                        new_limit(id, id + 1000, Side::Bid, 100, 1));
      },
      [](const ProcessState<Writer>& state) { consume_process_state(state); });
}

template <typename Writer>
Stats bench_matcher_full_fill(Config config) {
  return measure_scenario(
      config,
      [config] {
        auto state = std::make_unique<ProcessState<Writer>>(default_config);
        preload_matcher_asks(*state,
                             config.shape.batches *
                                 config.shape.commands_per_batch,
                             100, 1);
        return state;
      },
      [](ProcessState<Writer>& state, std::uint64_t, std::uint64_t) {
        const OrderId id = state.next_order_id++;
        process_checked(state,
                        new_limit(id, id + 1000, Side::Bid, 100, 1));
      },
      [](const ProcessState<Writer>& state) { consume_process_state(state); });
}

template <typename Writer>
Stats bench_matcher_partial_fill(Config config) {
  return measure_scenario(
      config,
      [config] {
        auto state = std::make_unique<ProcessState<Writer>>(default_config);
        preload_matcher_asks(*state, 1, 100,
                             static_cast<Quantity>(
                                 config.shape.batches *
                                     config.shape.commands_per_batch +
                                 config.warmup_commands + 1));
        return state;
      },
      [](ProcessState<Writer>& state, std::uint64_t, std::uint64_t) {
        const OrderId id = state.next_order_id++;
        process_checked(state,
                        new_limit(id, id + 1000, Side::Bid, 100, 1));
      },
      [](const ProcessState<Writer>& state) { consume_process_state(state); });
}

template <typename Writer>
struct RunState {
  RunState(std::vector<CommandEnvelope>&& commands_value,
           const OrderBookConfig& config)
      : commands(std::move(commands_value)),
        reader(commands),
        matcher(reader, writer, config) {}

  std::vector<CommandEnvelope> commands;
  ArrayCommandReader reader;
  Writer writer;
  Matcher<ArrayCommandReader, Writer> matcher;
};

template <typename Writer>
void consume_run_state(const RunState<Writer>& state) {
  const auto best_bid = state.matcher.book().best(Side::Bid);
  const auto best_ask = state.matcher.book().best(Side::Ask);
  g_sink += best_bid ? best_bid->id + best_bid->remaining : 1;
  g_sink += best_ask ? best_ask->id + best_ask->remaining : 1;
  g_sink += state.commands.size();
}

void consume_run_state(const RunState<CountingEventWriter>& state) {
  consume_run_state<CountingEventWriter>(state);
  g_sink += state.writer.accepted + state.writer.rejected +
            state.writer.trades + state.writer.rested + state.writer.done;
}

void consume_run_state(const RunState<SinkEventWriter>& state) {
  consume_run_state<SinkEventWriter>(state);
  g_sink += state.writer.accepted + state.writer.rejected +
            state.writer.trades + state.writer.rested + state.writer.done;
}

std::vector<CommandEnvelope>
make_full_fill_commands(std::uint64_t command_count) {
  std::vector<CommandEnvelope> commands;
  commands.reserve(static_cast<std::size_t>(command_count));
  for (std::uint64_t i = 0; i < command_count; ++i) {
    const OrderId id = static_cast<OrderId>(1'000'000 + i);
    commands.push_back(
        {command_count + i + 1,
         {benchmark_client_id,
          new_limit(id, id + 1000, Side::Bid, 100, 1)}});
  }
  return commands;
}

template <typename Writer>
std::unique_ptr<RunState<Writer>> make_full_fill_run_state(
    std::uint64_t command_count, const OrderBookConfig& config) {
  auto state = std::make_unique<RunState<Writer>>(
      make_full_fill_commands(command_count), config);
  for (std::uint64_t i = 0; i < command_count; ++i) {
    const OrderId id = static_cast<OrderId>(i + 1);
    const CommandEnvelope envelope{
        i + 1,
        {benchmark_client_id,
         new_limit(id, id + 1000, Side::Ask, 100, 1)}};
    const ProcessResult result = state->matcher.process(envelope);
    if (result.status != ProcessStatus::Continue) {
      std::abort();
    }
  }
  return state;
}

template <typename Writer>
void warmup_run(Config config) {
  auto state = make_full_fill_run_state<Writer>(
      config.shape.commands_per_batch, run_batch_config);
  const RunResult result = state->matcher.run();
  if (!result.ok()) {
    std::abort();
  }
  consume_run_state(*state);
}

template <typename Writer>
Stats bench_run_full_fill_stream(Config config) {
  warmup_run<Writer>(config);

  std::vector<std::unique_ptr<RunState<Writer>>> batch_states;
  batch_states.reserve(static_cast<std::size_t>(config.shape.batches));
  for (std::uint64_t batch = 0; batch < config.shape.batches; ++batch) {
    batch_states.push_back(make_full_fill_run_state<Writer>(
        config.shape.commands_per_batch, run_batch_config));
  }

  std::vector<double> samples;
  samples.reserve(static_cast<std::size_t>(config.shape.batches));
  double total_ns = 0.0;
  for (auto& state : batch_states) {
    const auto start = std::chrono::steady_clock::now();
    const RunResult result = state->matcher.run();
    const auto stop = std::chrono::steady_clock::now();
    if (!result.ok()) {
      std::abort();
    }
    const double elapsed = static_cast<double>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(stop - start)
            .count());
    total_ns += elapsed;
    samples.push_back(elapsed /
                      static_cast<double>(config.shape.commands_per_batch));
    consume_run_state(*state);
  }

  double throughput_ns = 0.0;
  {
    auto state = make_full_fill_run_state<Writer>(
        config.shape.batches * config.shape.commands_per_batch,
        default_config);
    const auto start = std::chrono::steady_clock::now();
    const RunResult result = state->matcher.run();
    const auto stop = std::chrono::steady_clock::now();
    if (!result.ok()) {
      std::abort();
    }
    throughput_ns = static_cast<double>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(stop - start)
            .count());
    consume_run_state(*state);
  }

  std::sort(samples.begin(), samples.end());
  const std::uint64_t iterations =
      config.shape.batches * config.shape.commands_per_batch;
  return {iterations,
          config.shape.batches,
          total_ns / static_cast<double>(iterations),
          (static_cast<double>(iterations) * 1'000'000'000.0) / throughput_ns,
          percentile(samples, 50.0),
          percentile(samples, 90.0),
          percentile(samples, 99.0),
          percentile(samples, 99.9),
          samples.back()};
}

struct Scenario {
  std::string_view name;
  Stats (*run)(Config);
};

Config parse_args(int argc, char** argv) {
  Config config;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--cpu" && i + 1 < argc) {
      config.requested_cpu = std::atoi(argv[++i]);
    } else if (arg == "--runs" && i + 1 < argc) {
      config.runs = std::atoi(argv[++i]);
    } else if (arg == "--batches" && i + 1 < argc) {
      config.shape.batches = static_cast<std::uint64_t>(
          std::strtoull(argv[++i], nullptr, 10));
    } else if (arg == "--commands-per-batch" && i + 1 < argc) {
      config.shape.commands_per_batch = static_cast<std::uint64_t>(
          std::strtoull(argv[++i], nullptr, 10));
    } else if (arg == "--warmup-commands" && i + 1 < argc) {
      config.warmup_commands = static_cast<std::uint64_t>(
          std::strtoull(argv[++i], nullptr, 10));
    } else if (arg == "--timer" && i + 1 < argc) {
      const std::string timer = argv[++i];
      if (timer != "steady") {
        std::cout << "warning=unsupported_timer requested=" << timer
                  << " using=steady\n";
      }
    }
  }
  return config;
}

} // namespace

int main(int argc, char** argv) {
  const Config config = parse_args(argc, argv);
  configure_windows_runtime(config);
  print_build_context(config);

  const std::vector<Scenario> scenarios{
      {"order_book_non_crossing", bench_order_book_non_crossing},
      {"matcher_null_non_crossing",
       bench_matcher_non_crossing<NullEventWriter>},
      {"matcher_counting_non_crossing",
       bench_matcher_non_crossing<CountingEventWriter>},
      {"matcher_sink_non_crossing",
       bench_matcher_non_crossing<SinkEventWriter>},
      {"order_book_full_fill", bench_order_book_full_fill},
      {"matcher_null_full_fill",
       bench_matcher_full_fill<NullEventWriter>},
      {"matcher_counting_full_fill",
       bench_matcher_full_fill<CountingEventWriter>},
      {"matcher_sink_full_fill",
       bench_matcher_full_fill<SinkEventWriter>},
      {"order_book_partial_fill", bench_order_book_partial_fill},
      {"matcher_null_partial_fill",
       bench_matcher_partial_fill<NullEventWriter>},
      {"matcher_counting_partial_fill",
       bench_matcher_partial_fill<CountingEventWriter>},
      {"matcher_sink_partial_fill",
       bench_matcher_partial_fill<SinkEventWriter>},
      {"run_null_full_fill_stream",
       bench_run_full_fill_stream<NullEventWriter>},
      {"run_counting_full_fill_stream",
       bench_run_full_fill_stream<CountingEventWriter>},
      {"run_sink_full_fill_stream",
       bench_run_full_fill_stream<SinkEventWriter>},
  };

  std::vector<Summary> summaries;
  summaries.reserve(scenarios.size());
  for (const Scenario& scenario : scenarios) {
    summaries.push_back({std::string(scenario.name), {}});
  }

  for (int run = 1; run <= config.runs; ++run) {
    for (std::size_t index = 0; index < scenarios.size(); ++index) {
      const Stats stats = scenarios[index].run(config);
      summaries[index].runs.push_back(stats);
      print_stats(scenarios[index].name, run, stats);
    }
  }

  print_summary(summaries);
  std::cout << "sink=" << g_sink << '\n';
  return 0;
}
