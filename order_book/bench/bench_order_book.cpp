#include <fexma/order_book/order_book.hpp>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <iostream>
#include <string_view>
#include <vector>

using namespace fexma::order_book;

namespace {

volatile std::uint64_t g_sink = 0;

struct Stats {
  std::uint64_t operations{};
  double total_ns{};
  double ns_per_op{};
  double ops_per_second{};
  double p50{};
  double p90{};
  double p99{};
  double p999{};
  double max{};
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
          total_ns,
          total_ns / static_cast<double>(operations),
          (static_cast<double>(operations) * 1'000'000'000.0) / total_ns,
          percentile(50.0),
          percentile(90.0),
          percentile(99.0),
          percentile(99.9),
          samples.back()};
}

void print_stats(std::string_view name, const Stats& stats) {
  std::cout << name << ": ops=" << stats.operations
            << " total_ns=" << static_cast<std::uint64_t>(stats.total_ns)
            << " ns/op=" << stats.ns_per_op
            << " ops/s=" << stats.ops_per_second
            << " p50=" << stats.p50
            << " p90=" << stats.p90
            << " p99=" << stats.p99
            << " p99.9=" << stats.p999
            << " max=" << stats.max << '\n';
}

OrderBook make_book(OrderSlot capacity = 300000) {
  OrderBook book({1, 4096, capacity});
  book.warm_up();
  return book;
}

void fill_level(OrderBook& book, OrderId& id, Side side, PriceTick price,
                std::uint32_t count) {
  for (std::uint32_t i = 0; i < count; ++i) {
    const PutResult result = book.put({id, id + 1000, side, price, 100});
    if (!result.ok()) {
      std::abort();
    }
    ++id;
  }
}

} // namespace

int main() {
  constexpr std::uint64_t batches = 200;
  constexpr std::uint64_t ops = 1000;

  {
    auto book = make_book();
    OrderId id = 1;
    fill_level(book, id, Side::Ask, 100, 1000);
    print_stats("put_existing_level",
                run_batches(batches, ops, [&](std::uint64_t, std::uint64_t op) {
                  const OrderId next = id++;
                  const auto result = book.put({next, next, Side::Ask, 100, 1});
                  g_sink += static_cast<std::uint64_t>(result.status);
                  if (!result.ok()) {
                    std::abort();
                  }
                  if ((op & 1U) == 0) {
                    (void)book.cancel(next);
                  }
                }));
  }

  {
    auto book = make_book();
    OrderId id = 1;
    print_stats("put_empty_level",
                run_batches(batches, ops, [&](std::uint64_t batch, std::uint64_t op) {
                  const PriceTick price =
                      1 + static_cast<PriceTick>((batch * ops + op) % 4096);
                  const OrderId next = id++;
                  const auto result = book.put({next, next, Side::Bid, price, 1});
                  g_sink += static_cast<std::uint64_t>(result.status);
                  if (!result.ok()) {
                    std::abort();
                  }
                  (void)book.cancel(next);
                }));
  }

  {
    auto book = make_book();
    OrderId id = 1;
    fill_level(book, id, Side::Ask, 100, 10000);
    print_stats("select_best_opposite",
                run_batches(batches, ops, [&](std::uint64_t, std::uint64_t) {
                  const auto best = book.select_best_opposite(Side::Bid);
                  g_sink += best ? best->id : 0;
                }));
  }

  {
    auto book = make_book();
    OrderId id = 1;
    fill_level(book, id, Side::Ask, 100, static_cast<std::uint32_t>(batches * ops));
    print_stats("decrement_selected_partial",
                run_batches(batches, ops, [&](std::uint64_t, std::uint64_t) {
                  const auto best = book.select_best_opposite(Side::Bid);
                  g_sink += best ? best->remaining : 0;
                  book.decrement_selected(1);
                }));
  }

  {
    auto book = make_book();
    OrderId id = 1;
    fill_level(book, id, Side::Ask, 100, static_cast<std::uint32_t>(batches * ops));
    print_stats("decrement_selected_full_same_level",
                run_batches(batches, ops, [&](std::uint64_t, std::uint64_t) {
                  const auto best = book.select_best_opposite(Side::Bid);
                  g_sink += best ? best->id : 0;
                  book.decrement_selected(100);
                }));
  }

  {
    auto book = make_book();
    OrderId id = 1;
    for (std::uint64_t i = 0; i < batches * ops; ++i) {
      const PriceTick price = 100 + static_cast<PriceTick>(i % 64);
      fill_level(book, id, Side::Ask, price, 1);
    }
    print_stats("decrement_selected_full_empty_level",
                run_batches(batches, ops, [&](std::uint64_t, std::uint64_t) {
                  const auto best = book.select_best_opposite(Side::Bid);
                  g_sink += best ? best->price : 0;
                  book.decrement_selected(100);
                }));
  }

  {
    auto book = make_book();
    OrderId id = 1;
    for (PriceTick price = 100; price < 164; ++price) {
      fill_level(book, id, Side::Ask, price, 1);
    }
    print_stats("next_price_inside_segment",
                run_batches(64, 1, [&](std::uint64_t, std::uint64_t) {
                  const auto best = book.select_best_opposite(Side::Bid);
                  g_sink += best ? best->price : 0;
                  book.decrement_selected(100);
                }));
  }

  {
    auto book = make_book();
    OrderId id = 1;
    for (PriceTick price = 120; price < 4096; price += 64) {
      fill_level(book, id, Side::Ask, price, 1);
    }
    print_stats("next_price_between_segments",
                run_batches(62, 1, [&](std::uint64_t, std::uint64_t) {
                  const auto best = book.select_best_opposite(Side::Bid);
                  g_sink += best ? best->price : 0;
                  book.decrement_selected(100);
                }));
  }

  {
    auto book = make_book();
    OrderId id = 1;
    for (PriceTick price = 1; price <= 4096; ++price) {
      fill_level(book, id, Side::Ask, price, 1);
    }
    print_stats("coldish_best_sweep",
                run_batches(4096, 1, [&](std::uint64_t, std::uint64_t) {
                  const auto best = book.select_best_opposite(Side::Bid);
                  g_sink += best ? best->price : 0;
                  book.decrement_selected(100);
                }));
  }

  {
    auto book = make_book();
    OrderId id = 1;
    fill_level(book, id, Side::Bid, 100, static_cast<std::uint32_t>(batches * ops));
    OrderId cancel_id = 1;
    print_stats("cancel_head",
                run_batches(batches, ops, [&](std::uint64_t, std::uint64_t) {
                  const auto result = book.cancel(cancel_id++);
                  g_sink += static_cast<std::uint64_t>(result.status);
                }));
  }

  {
    auto book = make_book();
    OrderId id = 1;
    fill_level(book, id, Side::Bid, 100,
               static_cast<std::uint32_t>(batches * ops + 2));
    OrderId cancel_id = 2;
    print_stats("cancel_middle",
                run_batches(batches, ops, [&](std::uint64_t, std::uint64_t) {
                  const auto result = book.cancel(cancel_id++);
                  g_sink += static_cast<std::uint64_t>(result.status);
                }));
  }

  {
    auto book = make_book();
    OrderId id = 1;
    fill_level(book, id, Side::Bid, 100, static_cast<std::uint32_t>(batches * ops));
    OrderId change_id = 1;
    print_stats("change_quantity",
                run_batches(batches, ops, [&](std::uint64_t, std::uint64_t) {
                  const auto result = book.change(change_id++, {50});
                  g_sink += static_cast<std::uint64_t>(result.status);
                }));
  }

  std::cout << "sink=" << g_sink << '\n';
  return 0;
}
