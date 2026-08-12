/**
 * @file bench_matcher.cpp
 * @brief Matcher cost-decomposition benchmark harness.
 *
 * The benchmark separates OrderBook-only work, Matcher::process orchestration,
 * EventWriter behavior, and Matcher::run reader-loop overhead. Setup,
 * allocation, command generation, and liquidity preload are outside timed
 * regions. Percentiles are computed from per-batch ns/command samples.
 */
#include <fexma/matcher/matcher.hpp>
#include <fexma/order_book/order_book.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string_view>
#include <vector>

using namespace fexma::matcher;

namespace {

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

struct BatchShape {
  std::uint64_t batches;
  std::uint64_t commands_per_batch;
};

constexpr OrderBookConfig default_config{1, 4096, 400000};
constexpr BatchShape steady_shape{300, 1000};
constexpr int benchmark_runs = 5;

class ShutdownCommandReader {
public:
  [[nodiscard]] CommandReadResult read_next() noexcept {
    return {CommandReadStatus::Ok, {CommandType::Shutdown, {}}};
  }
};

class ArrayCommandReader {
public:
  explicit ArrayCommandReader(const std::vector<Command>& commands)
      : commands_(commands) {}

  [[nodiscard]] CommandReadResult read_next() noexcept {
    if (next_ == commands_.size()) {
      return {CommandReadStatus::Ok, {CommandType::Shutdown, {}}};
    }
    return {CommandReadStatus::Ok, commands_[next_++]};
  }

private:
  const std::vector<Command>& commands_;
  std::size_t next_{};
};

class NullEventWriter {
public:
  [[nodiscard]] PublishResult publish(const Event&) noexcept {
    return {PublishStatus::Ok};
  }
};

class CountingEventWriter {
public:
  [[nodiscard]] PublishResult publish(const Event& event) noexcept {
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
  [[nodiscard]] PublishResult publish(const Event& event) noexcept {
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
  return {CommandType::NewLimit, {id, owner_id, side, price, quantity}};
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

template <typename SetupFn, typename OperationFn, typename ConsumeFn>
Stats measure_scenario(BatchShape shape, SetupFn&& setup,
                       OperationFn&& operation, ConsumeFn&& consume) {
  std::vector<double> samples;
  samples.reserve(static_cast<std::size_t>(shape.batches));
  double total_ns = 0.0;

  {
    auto state = setup();
    for (std::uint64_t batch = 0; batch < shape.batches; ++batch) {
      const auto start = std::chrono::steady_clock::now();
      for (std::uint64_t command = 0; command < shape.commands_per_batch;
           ++command) {
        operation(*state, batch, command);
      }
      const auto stop = std::chrono::steady_clock::now();
      const double elapsed = static_cast<double>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(stop - start)
              .count());
      total_ns += elapsed;
      samples.push_back(
          elapsed / static_cast<double>(shape.commands_per_batch));
    }
    consume(*state);
  }

  const std::uint64_t iterations =
      shape.batches * shape.commands_per_batch;
  double throughput_ns = 0.0;
  {
    auto state = setup();
    const auto start = std::chrono::steady_clock::now();
    for (std::uint64_t batch = 0; batch < shape.batches; ++batch) {
      for (std::uint64_t command = 0; command < shape.commands_per_batch;
           ++command) {
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
          samples.back()};
}

template <typename Fn>
void run_benchmark(std::string_view name, Fn&& make_stats) {
  for (int run = 1; run <= benchmark_runs; ++run) {
    print_stats(name, run, make_stats());
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
  std::cout << "NDEBUG=defined optimization=/O2\n";
#else
  std::cout << "NDEBUG=not_defined optimization=/Od\n";
#endif
  std::cout << "timer=std::chrono::steady_clock"
            << " runs=" << benchmark_runs
            << " steady_batches=" << steady_shape.batches
            << " steady_commands_per_batch="
            << steady_shape.commands_per_batch << '\n';
  std::cout << "latency_sample_unit=batch_ns_per_command"
            << " throughput_pass=separate_unsampled_run\n";
  std::cout << "default_config min_price_tick="
            << default_config.min_price_tick
            << " max_price_tick=" << default_config.max_price_tick
            << " max_orders=" << default_config.max_orders << '\n';
  std::cout << "dce_guard=state_consumed_after_timed_region"
            << " null_writer_no_sink_writes=true"
            << " sink_writer_has_volatile_publish_writes=true\n";
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

Stats bench_order_book_non_crossing() {
  return measure_scenario(
      steady_shape,
      [] { return std::make_unique<BookState>(default_config); },
      [](BookState& state, std::uint64_t, std::uint64_t) {
        const OrderId id = state.next_order_id++;
        insert_checked(state.book, id, id + 1000, Side::Bid, 100, 1);
      },
      consume_book_state);
}

Stats bench_order_book_full_fill() {
  return measure_scenario(
      steady_shape,
      [] {
        auto state = std::make_unique<BookState>(default_config);
        preload_book_asks(*state,
                          steady_shape.batches *
                              steady_shape.commands_per_batch,
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

Stats bench_order_book_partial_fill() {
  return measure_scenario(
      steady_shape,
      [] {
        auto state = std::make_unique<BookState>(default_config);
        preload_book_asks(*state, 1, 100,
                          static_cast<Quantity>(
                              steady_shape.batches *
                                  steady_shape.commands_per_batch +
                              1));
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
  const ProcessResult result = state.matcher.process(command);
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
Stats bench_matcher_non_crossing() {
  return measure_scenario(
      steady_shape,
      [] { return std::make_unique<ProcessState<Writer>>(default_config); },
      [](ProcessState<Writer>& state, std::uint64_t, std::uint64_t) {
        const OrderId id = state.next_order_id++;
        process_checked(state,
                        new_limit(id, id + 1000, Side::Bid, 100, 1));
      },
      [](const ProcessState<Writer>& state) { consume_process_state(state); });
}

template <typename Writer>
Stats bench_matcher_full_fill() {
  return measure_scenario(
      steady_shape,
      [] {
        auto state = std::make_unique<ProcessState<Writer>>(default_config);
        preload_matcher_asks(*state,
                             steady_shape.batches *
                                 steady_shape.commands_per_batch,
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
Stats bench_matcher_partial_fill() {
  return measure_scenario(
      steady_shape,
      [] {
        auto state = std::make_unique<ProcessState<Writer>>(default_config);
        preload_matcher_asks(*state, 1, 100,
                             static_cast<Quantity>(
                                 steady_shape.batches *
                                     steady_shape.commands_per_batch +
                                 1));
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
  RunState(std::vector<Command>&& commands_value,
           const OrderBookConfig& config)
      : commands(std::move(commands_value)),
        reader(commands),
        matcher(reader, writer, config) {}

  std::vector<Command> commands;
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

std::vector<Command> make_full_fill_commands(std::uint64_t command_count) {
  std::vector<Command> commands;
  commands.reserve(static_cast<std::size_t>(command_count));
  for (std::uint64_t i = 0; i < command_count; ++i) {
    const OrderId id = static_cast<OrderId>(1'000'000 + i);
    commands.push_back(new_limit(id, id + 1000, Side::Bid, 100, 1));
  }
  return commands;
}

template <typename Writer>
std::unique_ptr<RunState<Writer>> make_full_fill_run_state(
    std::uint64_t command_count) {
  auto state = std::make_unique<RunState<Writer>>(
      make_full_fill_commands(command_count), default_config);
  for (std::uint64_t i = 0; i < command_count; ++i) {
    const OrderId id = static_cast<OrderId>(i + 1);
    const ProcessResult result =
        state->matcher.process(new_limit(id, id + 1000, Side::Ask, 100, 1));
    if (result.status != ProcessStatus::Continue) {
      std::abort();
    }
  }
  return state;
}

template <typename Writer>
Stats bench_run_full_fill_stream() {
  constexpr std::uint64_t command_count =
      steady_shape.batches * steady_shape.commands_per_batch;
  std::vector<double> samples;
  samples.reserve(benchmark_runs);
  double total_ns = 0.0;
  double throughput_ns = 0.0;

  for (int run = 0; run < benchmark_runs; ++run) {
    auto state = make_full_fill_run_state<Writer>(command_count);
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
    samples.push_back(elapsed / static_cast<double>(command_count));
    consume_run_state(*state);
  }

  {
    auto state = make_full_fill_run_state<Writer>(command_count);
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
  const auto percentile = [&samples](double p) {
    const std::size_t index = static_cast<std::size_t>(
        (static_cast<double>(samples.size() - 1) * p) / 100.0);
    return samples[index];
  };
  return {command_count * benchmark_runs,
          benchmark_runs,
          total_ns / static_cast<double>(command_count * benchmark_runs),
          (static_cast<double>(command_count) * 1'000'000'000.0) /
              throughput_ns,
          percentile(50.0),
          percentile(90.0),
          percentile(99.0),
          percentile(99.9),
          samples.back()};
}

} // namespace

int main() {
  print_build_context();

  run_benchmark("order_book_non_crossing",
                bench_order_book_non_crossing);
  run_benchmark("matcher_null_non_crossing",
                bench_matcher_non_crossing<NullEventWriter>);
  run_benchmark("matcher_counting_non_crossing",
                bench_matcher_non_crossing<CountingEventWriter>);
  run_benchmark("matcher_sink_non_crossing",
                bench_matcher_non_crossing<SinkEventWriter>);

  run_benchmark("order_book_full_fill", bench_order_book_full_fill);
  run_benchmark("matcher_null_full_fill",
                bench_matcher_full_fill<NullEventWriter>);
  run_benchmark("matcher_counting_full_fill",
                bench_matcher_full_fill<CountingEventWriter>);
  run_benchmark("matcher_sink_full_fill",
                bench_matcher_full_fill<SinkEventWriter>);

  run_benchmark("order_book_partial_fill",
                bench_order_book_partial_fill);
  run_benchmark("matcher_null_partial_fill",
                bench_matcher_partial_fill<NullEventWriter>);
  run_benchmark("matcher_counting_partial_fill",
                bench_matcher_partial_fill<CountingEventWriter>);
  run_benchmark("matcher_sink_partial_fill",
                bench_matcher_partial_fill<SinkEventWriter>);

  run_benchmark("run_null_full_fill_stream",
                bench_run_full_fill_stream<NullEventWriter>);
  run_benchmark("run_counting_full_fill_stream",
                bench_run_full_fill_stream<CountingEventWriter>);
  run_benchmark("run_sink_full_fill_stream",
                bench_run_full_fill_stream<SinkEventWriter>);

  std::cout << "sink=" << g_sink << '\n';
  return 0;
}
