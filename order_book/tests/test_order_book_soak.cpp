/**
 * @file test_order_book_soak.cpp
 * @brief Reproducible long-running differential and lifecycle validation.
 */
#include <fexma/order_book/order_book.hpp>

#include "reference_order_book.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <exception>
#include <iomanip>
#include <iostream>
#include <limits>
#include <new>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <Psapi.h>
#include <malloc.h>
#else
#include <sys/resource.h>
#include <unistd.h>
#endif

using namespace fexma::order_book;
using namespace fexma::order_book::test_support;

namespace allocation_guard {
inline std::uint64_t allocation_count{};
inline bool enabled{};

void* allocate(std::size_t size, std::size_t alignment = 0) {
  if (enabled) {
    ++allocation_count;
  }
  if (alignment == 0) {
    if (void* memory = std::malloc(size)) {
      return memory;
    }
  } else {
    const std::size_t rounded =
        ((size + alignment - 1) / alignment) * alignment;
#ifdef _WIN32
    if (void* memory = _aligned_malloc(rounded, alignment)) {
      return memory;
    }
#else
    if (void* memory = std::aligned_alloc(alignment, rounded)) {
      return memory;
    }
#endif
  }
  throw std::bad_alloc();
}
} // namespace allocation_guard

void* operator new(std::size_t size) {
  return allocation_guard::allocate(size);
}

void* operator new[](std::size_t size) {
  return allocation_guard::allocate(size);
}

void* operator new(std::size_t size, std::align_val_t alignment) {
  return allocation_guard::allocate(size, static_cast<std::size_t>(alignment));
}

void* operator new[](std::size_t size, std::align_val_t alignment) {
  return allocation_guard::allocate(size, static_cast<std::size_t>(alignment));
}

void operator delete(void* memory) noexcept { std::free(memory); }

void operator delete[](void* memory) noexcept { std::free(memory); }

void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }

void operator delete[](void* memory, std::size_t) noexcept {
  std::free(memory);
}

void operator delete(void* memory, std::align_val_t) noexcept {
#ifdef _WIN32
  _aligned_free(memory);
#else
  std::free(memory);
#endif
}

void operator delete[](void* memory, std::align_val_t) noexcept {
#ifdef _WIN32
  _aligned_free(memory);
#else
  std::free(memory);
#endif
}

void operator delete(void* memory, std::size_t,
                     std::align_val_t alignment) noexcept {
  ::operator delete(memory, alignment);
}

void operator delete[](void* memory, std::size_t,
                       std::align_val_t alignment) noexcept {
  ::operator delete[](memory, alignment);
}

namespace {

enum class Mode { Quick, Stress, Soak };
enum class Profile {
  Failures,
  DenseFifo,
  Mixed,
  DuplicateHeavy,
  MissHeavy,
  BitmapBoundary,
  WideSparse,
  NearCapacity,
  Asymmetric
};
enum class Operation : std::uint8_t { Insert, SetRemaining, Erase, Best };

struct Options {
  Mode mode{Mode::Quick};
  std::uint64_t operations{};
  std::uint64_t duration_seconds{};
  std::uint64_t validation_interval{};
  std::vector<std::uint64_t> seeds;
  std::string scenario_name;
};

struct Scenario {
  const char* name;
  OrderBookConfig config;
  Profile profile;
};

constexpr std::array<Scenario, 10> scenarios{{
    {"capacity_zero", {10, 20, 0}, Profile::Failures},
    {"single_price_capacity_one", {42, 42, 1}, Profile::DenseFifo},
    {"single_segment_capacity_two", {1, 63, 2}, Profile::DuplicateHeavy},
    {"segment_boundary", {63, 65, 7}, Profile::Mixed},
    {"bitmap_word_boundary", {1, 4160, 31}, Profile::BitmapBoundary},
    {"unaligned_1_4096", {1, 4096, 127}, Profile::MissHeavy},
    {"wide_sparse", {17, 1'000'000, 257}, Profile::WideSparse},
    {"dense_fifo", {100, 100, 257}, Profile::DenseFifo},
    {"near_capacity_non_power", {64, 255, 4093}, Profile::NearCapacity},
    {"asymmetric_sides", {1, 4096, 521}, Profile::Asymmetric},
}};

constexpr std::array<std::uint64_t, 3> default_seeds{
    0x0000000000C0FFEEULL, 0x123456789ABCDEF0ULL, 0xDEADBEEF51515151ULL};
constexpr std::size_t trace_capacity = 64;

struct TraceEntry {
  std::uint64_t step{};
  Operation operation{};
  OrderId id{};
  OwnerId owner{};
  Side side{};
  PriceTick price{};
  Quantity quantity{};
};

class Trace {
public:
  void push(const TraceEntry& entry) noexcept {
    entries_[next_] = entry;
    next_ = (next_ + 1U) % entries_.size();
    size_ = (std::min)(size_ + 1U, entries_.size());
  }

  void print(std::ostream& output) const {
    const std::size_t first =
        (next_ + entries_.size() - size_) % entries_.size();
    for (std::size_t i = 0; i < size_; ++i) {
      const TraceEntry& entry = entries_[(first + i) % entries_.size()];
      output << "trace step=" << entry.step << " op=";
      switch (entry.operation) {
      case Operation::Insert:
        output << "insert";
        break;
      case Operation::SetRemaining:
        output << "set_remaining";
        break;
      case Operation::Erase:
        output << "erase";
        break;
      case Operation::Best:
        output << "best";
        break;
      }
      output << " id=" << entry.id << " owner=" << entry.owner
             << " side=" << (entry.side == Side::Bid ? "Bid" : "Ask")
             << " price=" << entry.price << " quantity=" << entry.quantity
             << '\n';
    }
  }

private:
  std::array<TraceEntry, trace_capacity> entries_{};
  std::size_t next_{};
  std::size_t size_{};
};

class ActiveIds {
public:
  explicit ActiveIds(OrderCapacity capacity) {
    ids_.reserve(capacity);
    positions_.reserve(static_cast<std::size_t>(capacity) * 2U + 1U);
  }

  [[nodiscard]] bool empty() const noexcept { return ids_.empty(); }
  [[nodiscard]] std::size_t size() const noexcept { return ids_.size(); }

  [[nodiscard]] OrderId random(std::mt19937_64& random) const noexcept {
    return ids_[static_cast<std::size_t>(random() % ids_.size())];
  }

  void insert(OrderId id) {
    positions_.emplace(id, ids_.size());
    ids_.push_back(id);
  }

  void erase(OrderId id) {
    const auto found = positions_.find(id);
    if (found == positions_.end()) {
      return;
    }
    const std::size_t position = found->second;
    const OrderId last = ids_.back();
    ids_[position] = last;
    positions_[last] = position;
    ids_.pop_back();
    positions_.erase(found);
  }

private:
  std::vector<OrderId> ids_;
  std::unordered_map<OrderId, std::size_t> positions_;
};

struct MemoryUsage {
  std::uint64_t resident_bytes{};
  std::uint64_t peak_resident_bytes{};
};

[[nodiscard]] MemoryUsage memory_usage() noexcept {
#ifdef _WIN32
  PROCESS_MEMORY_COUNTERS_EX counters{};
  counters.cb = sizeof(counters);
  if (GetProcessMemoryInfo(
          GetCurrentProcess(),
          reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
          sizeof(counters)) == FALSE) {
    return {};
  }
  return {static_cast<std::uint64_t>(counters.WorkingSetSize),
          static_cast<std::uint64_t>(counters.PeakWorkingSetSize)};
#else
  std::uint64_t resident = 0;
  if (FILE* statm = std::fopen("/proc/self/statm", "r")) {
    unsigned long resident_pages = 0;
    if (std::fscanf(statm, "%*lu %lu", &resident_pages) == 1) {
      resident = static_cast<std::uint64_t>(resident_pages) *
                 static_cast<std::uint64_t>(sysconf(_SC_PAGESIZE));
    }
    std::fclose(statm);
  }
  rusage usage{};
  const std::uint64_t peak =
      getrusage(RUSAGE_SELF, &usage) == 0
          ? static_cast<std::uint64_t>(usage.ru_maxrss) * 1024U
          : 0;
  return {resident, peak};
#endif
}

[[nodiscard]] bool same_order(const OrderView& actual,
                              const ReferenceOrderView& expected) noexcept {
  return actual.id == expected.id && actual.owner_id == expected.owner_id &&
         actual.side == expected.side && actual.price == expected.price &&
         actual.remaining == expected.remaining;
}

[[nodiscard]] bool
same_best(const std::optional<OrderView>& actual,
          const std::optional<ReferenceOrderView>& expected) {
  return actual.has_value() == expected.has_value() &&
         (!actual || same_order(*actual, *expected));
}

[[nodiscard]] bool
same_actual_best(const std::optional<OrderView>& lhs,
                 const std::optional<OrderView>& rhs) noexcept {
  return lhs.has_value() == rhs.has_value() &&
         (!lhs || (lhs->id == rhs->id && lhs->owner_id == rhs->owner_id &&
                   lhs->side == rhs->side && lhs->price == rhs->price &&
                   lhs->remaining == rhs->remaining));
}

[[nodiscard]] bool same_insert(InsertStatus actual,
                               ReferenceInsertStatus expected) noexcept {
  switch (expected) {
  case ReferenceInsertStatus::Ok:
    return actual == InsertStatus::Ok;
  case ReferenceInsertStatus::DuplicateOrderId:
    return actual == InsertStatus::DuplicateOrderId;
  case ReferenceInsertStatus::CapacityExhausted:
    return actual == InsertStatus::CapacityExhausted;
  case ReferenceInsertStatus::PriceOutOfRange:
    return actual == InsertStatus::PriceOutOfRange;
  case ReferenceInsertStatus::InvalidQuantity:
    return actual == InsertStatus::InvalidQuantity;
  }
  return false;
}

[[nodiscard]] bool same_set(SetRemainingStatus actual,
                            ReferenceSetRemainingStatus expected) noexcept {
  switch (expected) {
  case ReferenceSetRemainingStatus::Ok:
    return actual == SetRemainingStatus::Ok;
  case ReferenceSetRemainingStatus::NotFound:
    return actual == SetRemainingStatus::NotFound;
  case ReferenceSetRemainingStatus::InvalidQuantity:
    return actual == SetRemainingStatus::InvalidQuantity;
  }
  return false;
}

[[nodiscard]] bool same_erase(EraseStatus actual,
                              ReferenceEraseStatus expected) noexcept {
  switch (expected) {
  case ReferenceEraseStatus::Ok:
    return actual == EraseStatus::Ok;
  case ReferenceEraseStatus::NotFound:
    return actual == EraseStatus::NotFound;
  }
  return false;
}

[[nodiscard]] bool same_bests(const OrderBook& actual,
                              const ReferenceOrderBook& expected) {
  return same_best(actual.best(Side::Bid), expected.best(Side::Bid)) &&
         same_best(actual.best(Side::Ask), expected.best(Side::Ask));
}

[[nodiscard]] std::uint64_t parse_integer(std::string_view text) {
  std::size_t parsed = 0;
  const std::string owned(text);
  const std::uint64_t value = std::stoull(owned, &parsed, 0);
  if (parsed != owned.size()) {
    throw std::invalid_argument("invalid integer: " + owned);
  }
  return value;
}

[[nodiscard]] Options parse_options(int argc, char* argv[]) {
  Options options;
  bool custom_operations = false;
  for (int i = 1; i < argc; ++i) {
    const std::string_view argument(argv[i]);
    const auto require_value =
        [&](std::string_view option) -> std::string_view {
      if (++i >= argc) {
        throw std::invalid_argument("missing value for " + std::string(option));
      }
      return argv[i];
    };
    if (argument == "--mode") {
      const std::string_view mode = require_value(argument);
      if (mode == "quick") {
        options.mode = Mode::Quick;
      } else if (mode == "stress") {
        options.mode = Mode::Stress;
      } else if (mode == "soak") {
        options.mode = Mode::Soak;
      } else {
        throw std::invalid_argument("unknown mode: " + std::string(mode));
      }
    } else if (argument == "--operations") {
      options.operations = parse_integer(require_value(argument));
      custom_operations = true;
    } else if (argument == "--duration-seconds") {
      options.duration_seconds = parse_integer(require_value(argument));
    } else if (argument == "--validation-interval") {
      options.validation_interval = parse_integer(require_value(argument));
    } else if (argument == "--seed") {
      options.seeds.push_back(parse_integer(require_value(argument)));
    } else if (argument == "--scenario") {
      options.scenario_name = require_value(argument);
    } else if (argument == "--help") {
      std::cout << "Usage: test_order_book_soak [--mode quick|stress|soak] "
                   "[--operations N] [--duration-seconds N] [--seed N] "
                   "[--validation-interval N] [--scenario NAME]\n";
      std::exit(0);
    } else {
      throw std::invalid_argument("unknown argument: " + std::string(argument));
    }
  }

  if (!custom_operations) {
    options.operations = options.mode == Mode::Quick    ? 600'000U
                         : options.mode == Mode::Stress ? 6'000'000U
                                                        : 100'000'000U;
  }
  if (options.validation_interval == 0) {
    options.validation_interval = options.mode == Mode::Quick    ? 4'096U
                                  : options.mode == Mode::Stress ? 65'536U
                                                                 : 1'000'000U;
  }
  if (options.seeds.empty()) {
    options.seeds.assign(default_seeds.begin(), default_seeds.end());
  }
  return options;
}

[[nodiscard]] const char* mode_name(Mode mode) noexcept {
  switch (mode) {
  case Mode::Quick:
    return "quick";
  case Mode::Stress:
    return "stress";
  case Mode::Soak:
    return "soak";
  }
  return "unknown";
}

[[nodiscard]] std::size_t bucket_count_for(OrderCapacity capacity) noexcept {
  std::size_t count = 1;
  const std::size_t required =
      capacity == 0 ? 1 : static_cast<std::size_t>(capacity) * 2U + 1U;
  while (count < required) {
    count <<= 1U;
  }
  return count;
}

[[nodiscard]] std::size_t index_hash(OrderId id) noexcept {
  std::uint64_t value = id;
  value ^= value >> 33U;
  value *= 0xff51afd7ed558ccdULL;
  value ^= value >> 33U;
  value *= 0xc4ceb9fe1a85ec53ULL;
  value ^= value >> 33U;
  return static_cast<std::size_t>(value);
}

class DifferentialRun {
public:
  DifferentialRun(const Scenario& scenario, std::uint64_t seed,
                  std::uint64_t validation_interval)
      : scenario_(scenario), validation_interval_(validation_interval),
        random_(seed), actual_(scenario.config), expected_(scenario.config),
        active_(scenario.config.max_orders),
        collision_bucket_(bucket_count_for(scenario.config.max_orders) - 1U) {}

  [[nodiscard]] bool run(std::uint64_t operations) {
    for (std::uint64_t local_step = 0; local_step < operations; ++local_step) {
      step_ = local_step;
      if (!execute_one()) {
        return false;
      }
      if ((local_step + 1U) % validation_interval_ == 0 &&
          !validate("periodic")) {
        return false;
      }
    }
    return final_validation();
  }

  [[nodiscard]] const Trace& trace() const noexcept { return trace_; }
  [[nodiscard]] const char* error() const noexcept { return error_; }

private:
  [[nodiscard]] bool fail(const char* reason) noexcept {
    error_ = reason;
    return false;
  }

  [[nodiscard]] bool validate(const char* reason) {
    if (!actual_.validate_invariants() || !expected_.validate_invariants() ||
        !same_bests(actual_, expected_) ||
        active_.size() != expected_.active_order_count()) {
      return fail(reason);
    }
    return true;
  }

  [[nodiscard]] Side random_side() noexcept {
    if (scenario_.profile == Profile::Asymmetric) {
      return random_() % 20U == 0 ? Side::Ask : Side::Bid;
    }
    return (random_() & 1U) == 0 ? Side::Bid : Side::Ask;
  }

  [[nodiscard]] PriceTick random_price() noexcept {
    const OrderBookConfig& config = scenario_.config;
    if (scenario_.profile == Profile::DenseFifo) {
      return config.min_price_tick;
    }
    if (scenario_.profile == Profile::BitmapBoundary) {
      constexpr std::array<PriceTick, 7> boundaries{1,    63,   64,  4031,
                                                    4032, 4096, 4160};
      return boundaries[static_cast<std::size_t>(random_() %
                                                 boundaries.size())];
    }
    const std::uint64_t width =
        static_cast<std::uint64_t>(config.max_price_tick) -
        config.min_price_tick + 1U;
    return static_cast<PriceTick>(config.min_price_tick + random_() % width);
  }

  [[nodiscard]] Quantity random_quantity() noexcept {
    if ((random_() & 31U) == 0) {
      return (std::numeric_limits<Quantity>::max)();
    }
    if ((random_() & 63U) == 0) {
      return 1;
    }
    return static_cast<Quantity>(1U + random_() % 100'000U);
  }

  [[nodiscard]] OrderId next_colliding_id() noexcept {
    const std::size_t mask = bucket_count_for(scenario_.config.max_orders) - 1U;
    while ((index_hash(collision_cursor_) & mask) != collision_bucket_) {
      ++collision_cursor_;
    }
    return collision_cursor_++;
  }

  [[nodiscard]] OrderId next_new_id() noexcept {
    if (scenario_.profile == Profile::BitmapBoundary) {
      return next_colliding_id();
    }
    if (scenario_.profile == Profile::WideSparse && (random_() & 1U) != 0) {
      return random_();
    }
    return next_sequential_id_++;
  }

  [[nodiscard]] int operation_choice() noexcept {
    if (scenario_.profile == Profile::Failures) {
      return static_cast<int>(random_() % 100U);
    }
    if (scenario_.profile == Profile::NearCapacity) {
      const std::size_t target =
          static_cast<std::size_t>(scenario_.config.max_orders) * 95U / 100U;
      if (active_.size() < target) {
        return static_cast<int>(random_() % 60U);
      }
      return static_cast<int>(40U + random_() % 60U);
    }
    if (scenario_.profile == Profile::DuplicateHeavy) {
      return static_cast<int>(random_() % 55U);
    }
    if (scenario_.profile == Profile::MissHeavy) {
      return static_cast<int>(55U + random_() % 45U);
    }
    return static_cast<int>(random_() % 100U);
  }

  [[nodiscard]] bool execute_one() {
    const int choice = operation_choice();
    if (choice < 42 || active_.empty()) {
      return execute_insert();
    }
    if (choice < 67) {
      return execute_set_remaining();
    }
    if (choice < 92) {
      return execute_erase();
    }
    return execute_best();
  }

  [[nodiscard]] bool execute_insert() {
    const auto actual_bid_before = actual_.best(Side::Bid);
    const auto actual_ask_before = actual_.best(Side::Ask);
    const std::size_t active_before = active_.size();

    RestingOrderData order{next_new_id(), random_(), random_side(),
                           random_price(), random_quantity()};
    if (!active_.empty() && (scenario_.profile == Profile::DuplicateHeavy ||
                             random_() % 12U == 0)) {
      order.id = active_.random(random_);
    }
    if (random_() % 31U == 0) {
      order.quantity = 0;
    } else if (random_() % 37U == 0) {
      order.price = scenario_.config.min_price_tick == 0
                        ? scenario_.config.max_price_tick + 1U
                        : scenario_.config.min_price_tick - 1U;
    }

    trace_.push({step_, Operation::Insert, order.id, order.owner_id, order.side,
                 order.price, order.quantity});
    const InsertResult actual_result = actual_.insert(order);
    const ReferenceInsertResult expected_result = expected_.insert(order);
    if (!same_insert(actual_result.status, expected_result.status)) {
      return fail("insert result mismatch");
    }
    if (actual_result.ok()) {
      active_.insert(order.id);
    } else if (step_ % 257U == 0 &&
               (active_.size() != active_before ||
                !same_actual_best(actual_.best(Side::Bid), actual_bid_before) ||
                !same_actual_best(actual_.best(Side::Ask),
                                  actual_ask_before))) {
      return fail("failed insert changed visible state");
    }
    return true;
  }

  [[nodiscard]] bool execute_set_remaining() {
    const bool missing = active_.empty() || random_() % 4U == 0;
    const OrderId id = missing ? (std::numeric_limits<OrderId>::max)() - step_
                               : active_.random(random_);
    Quantity quantity = random_quantity();
    if (random_() % 17U == 0) {
      quantity = 0;
    }
    const auto bid_before = actual_.best(Side::Bid);
    const auto ask_before = actual_.best(Side::Ask);
    trace_.push(
        {step_, Operation::SetRemaining, id, 0, Side::Bid, 0, quantity});
    const SetRemainingResult actual_result =
        actual_.set_remaining(id, quantity);
    const ReferenceSetRemainingResult expected_result =
        expected_.set_remaining(id, quantity);
    if (!same_set(actual_result.status, expected_result.status) ||
        actual_result.previous_remaining !=
            expected_result.previous_remaining) {
      return fail("set_remaining result mismatch");
    }
    if (!actual_result.ok() && step_ % 257U == 0 &&
        (!same_actual_best(actual_.best(Side::Bid), bid_before) ||
         !same_actual_best(actual_.best(Side::Ask), ask_before))) {
      return fail("failed set_remaining changed visible state");
    }
    return true;
  }

  [[nodiscard]] bool execute_erase() {
    const bool missing = active_.empty() || random_() % 4U == 0;
    OrderId id = missing ? (std::numeric_limits<OrderId>::max)() - step_
                         : active_.random(random_);
    if (!missing && random_() % 4U == 0) {
      const Side side = random_side();
      const auto best = expected_.best(side);
      if (best) {
        id = best->id;
      }
    }
    const auto bid_before = actual_.best(Side::Bid);
    const auto ask_before = actual_.best(Side::Ask);
    trace_.push({step_, Operation::Erase, id, 0, Side::Bid, 0, 0});
    const EraseResult actual_result = actual_.erase(id);
    const ReferenceEraseResult expected_result = expected_.erase(id);
    if (!same_erase(actual_result.status, expected_result.status) ||
        (actual_result.ok() &&
         !same_order(actual_result.removed, expected_result.removed))) {
      return fail("erase result mismatch");
    }
    if (actual_result.ok()) {
      active_.erase(id);
    } else if (step_ % 257U == 0 &&
               (!same_actual_best(actual_.best(Side::Bid), bid_before) ||
                !same_actual_best(actual_.best(Side::Ask), ask_before))) {
      return fail("failed erase changed visible state");
    }
    return true;
  }

  [[nodiscard]] bool execute_best() {
    const Side side = random_side();
    trace_.push({step_, Operation::Best, 0, 0, side, 0, 0});
    return same_best(actual_.best(side), expected_.best(side))
               ? true
               : fail("best mismatch");
  }

  [[nodiscard]] bool final_validation() {
    if (!validate("final invariants")) {
      return false;
    }
    for (Side side : {Side::Bid, Side::Ask}) {
      while (true) {
        const auto actual_best = actual_.best(side);
        const auto expected_best = expected_.best(side);
        if (!same_best(actual_best, expected_best)) {
          return fail("final drain best mismatch");
        }
        if (!actual_best) {
          break;
        }
        const EraseResult actual_erased = actual_.erase(actual_best->id);
        const ReferenceEraseResult expected_erased =
            expected_.erase(actual_best->id);
        if (!actual_erased.ok() || !expected_erased.ok() ||
            !same_order(actual_erased.removed, expected_erased.removed)) {
          return fail("final drain erase mismatch");
        }
      }
    }
    return actual_.validate_invariants() && expected_.validate_invariants();
  }

  const Scenario& scenario_;
  std::uint64_t validation_interval_{};
  std::mt19937_64 random_;
  OrderBook actual_;
  ReferenceOrderBook expected_;
  ActiveIds active_;
  Trace trace_;
  std::uint64_t step_{};
  OrderId next_sequential_id_{1};
  OrderId collision_cursor_{1};
  std::size_t collision_bucket_{};
  const char* error_{"unknown"};
};

[[nodiscard]] bool drain_matches(OrderBook& actual,
                                 ReferenceOrderBook& expected) {
  for (Side side : {Side::Bid, Side::Ask}) {
    while (true) {
      const auto actual_best = actual.best(side);
      const auto expected_best = expected.best(side);
      if (!same_best(actual_best, expected_best)) {
        return false;
      }
      if (!actual_best) {
        break;
      }
      const auto actual_erased = actual.erase(actual_best->id);
      const auto expected_erased = expected.erase(actual_best->id);
      if (!actual_erased.ok() || !expected_erased.ok() ||
          !same_order(actual_erased.removed, expected_erased.removed)) {
        return false;
      }
    }
  }
  return actual.validate_invariants() && expected.validate_invariants();
}

template <typename ActualOperation, typename ExpectedOperation,
          typename CompareResult>
[[nodiscard]] bool atomic_failure_case(ActualOperation actual_operation,
                                       ExpectedOperation expected_operation,
                                       CompareResult compare_result) {
  OrderBook actual({64, 192, 4});
  ReferenceOrderBook expected({64, 192, 4});
  constexpr std::array<RestingOrderData, 4> orders{
      {{1, 11, Side::Bid, 128, 10},
       {2, 12, Side::Bid, 128, 20},
       {3, 13, Side::Ask, 96, 30},
       {4, 14, Side::Ask, 160, 40}}};
  for (const auto& order : orders) {
    if (!actual.insert(order).ok() || !expected.insert(order).ok()) {
      return false;
    }
  }
  if (!compare_result(actual_operation(actual), expected_operation(expected))) {
    return false;
  }
  return drain_matches(actual, expected);
}

[[nodiscard]] bool failure_atomicity_tests() {
  const auto compare_insert = [](const InsertResult& actual,
                                 const ReferenceInsertResult& expected) {
    return !actual.ok() && !expected.ok() &&
           same_insert(actual.status, expected.status);
  };
  const auto compare_set = [](const SetRemainingResult& actual,
                              const ReferenceSetRemainingResult& expected) {
    return !actual.ok() && !expected.ok() &&
           same_set(actual.status, expected.status) &&
           actual.previous_remaining == expected.previous_remaining;
  };
  const auto compare_erase = [](const EraseResult& actual,
                                const ReferenceEraseResult& expected) {
    return !actual.ok() && !expected.ok() &&
           same_erase(actual.status, expected.status);
  };

  return atomic_failure_case(
             [](OrderBook& book) {
               return book.insert({5, 15, Side::Bid, 100, 0});
             },
             [](ReferenceOrderBook& book) {
               return book.insert({5, 15, Side::Bid, 100, 0});
             },
             compare_insert) &&
         atomic_failure_case(
             [](OrderBook& book) {
               return book.insert({5, 15, Side::Bid, 63, 1});
             },
             [](ReferenceOrderBook& book) {
               return book.insert({5, 15, Side::Bid, 63, 1});
             },
             compare_insert) &&
         atomic_failure_case(
             [](OrderBook& book) {
               return book.insert({1, 99, Side::Ask, 100, 1});
             },
             [](ReferenceOrderBook& book) {
               return book.insert({1, 99, Side::Ask, 100, 1});
             },
             compare_insert) &&
         atomic_failure_case(
             [](OrderBook& book) {
               return book.insert({5, 15, Side::Bid, 100, 1});
             },
             [](ReferenceOrderBook& book) {
               return book.insert({5, 15, Side::Bid, 100, 1});
             },
             compare_insert) &&
         atomic_failure_case(
             [](OrderBook& book) { return book.set_remaining(1, 0); },
             [](ReferenceOrderBook& book) { return book.set_remaining(1, 0); },
             compare_set) &&
         atomic_failure_case(
             [](OrderBook& book) { return book.set_remaining(999, 1); },
             [](ReferenceOrderBook& book) {
               return book.set_remaining(999, 1);
             },
             compare_set) &&
         atomic_failure_case(
             [](OrderBook& book) { return book.erase(999); },
             [](ReferenceOrderBook& book) { return book.erase(999); },
             compare_erase);
}

[[nodiscard]] bool aggregate_quantity_test() {
  OrderBook actual({100, 100, 3});
  ReferenceOrderBook expected({100, 100, 3});
  const Quantity maximum = (std::numeric_limits<Quantity>::max)();
  for (const RestingOrderData order :
       {RestingOrderData{1, 1, Side::Ask, 100, maximum},
        RestingOrderData{2, 2, Side::Ask, 100, maximum},
        RestingOrderData{3, 3, Side::Ask, 100, 1}}) {
    if (!actual.insert(order).ok() || !expected.insert(order).ok()) {
      return false;
    }
  }
  return actual.validate_invariants() && expected.validate_invariants() &&
         drain_matches(actual, expected);
}

[[nodiscard]] bool allocation_test() {
  OrderBook book({1, 4160, 64});
  (void)book.insert({1, 1, Side::Bid, 4096, 10});
  (void)book.insert({2, 2, Side::Ask, 64, 20});
  allocation_guard::allocation_count = 0;
  allocation_guard::enabled = true;
  (void)book.insert({3, 3, Side::Bid, 4096, 30});
  (void)book.best(Side::Bid);
  (void)book.best(Side::Ask);
  (void)book.set_remaining(1, (std::numeric_limits<Quantity>::max)());
  (void)book.set_remaining(999, 1);
  (void)book.erase(2);
  (void)book.erase(999);
  (void)book.insert({4, 4, Side::Ask, 0, 1});
  (void)book.insert({1, 4, Side::Ask, 64, 1});
  allocation_guard::enabled = false;
  return allocation_guard::allocation_count == 0 && book.validate_invariants();
}

[[nodiscard]] bool lifecycle_tests() {
  for (int iteration = 0; iteration < 250; ++iteration) {
    OrderBook book({1, 4096, 31});
    for (OrderId id = 1; id <= 31; ++id) {
      if (!book.insert({id, id, id % 2 == 0 ? Side::Bid : Side::Ask,
                        static_cast<PriceTick>(1 + id * 127U),
                        static_cast<Quantity>(id)})
               .ok()) {
        return false;
      }
    }
    if (!book.validate_invariants()) {
      return false;
    }
  }

  OrderBook source({64, 192, 8});
  (void)source.insert({1, 11, Side::Bid, 128, 10});
  (void)source.insert({2, 12, Side::Ask, 96, 20});
  OrderBook moved(std::move(source));
  if (!moved.validate_invariants() || moved.best(Side::Bid)->id != 1 ||
      moved.best(Side::Ask)->id != 2) {
    return false;
  }
  OrderBook assigned({1, 1, 1});
  assigned = std::move(moved);
  return assigned.validate_invariants() && assigned.best(Side::Bid)->id == 1 &&
         assigned.best(Side::Ask)->id == 2;
}

[[nodiscard]] bool run_scenario(const Scenario& scenario, std::uint64_t seed,
                                std::uint64_t operations,
                                std::uint64_t validation_interval) {
  const auto started = std::chrono::steady_clock::now();
  DifferentialRun run(scenario, seed, validation_interval);
  if (!run.run(operations)) {
    std::cerr << "FAIL scenario=" << scenario.name << " seed=0x" << std::hex
              << seed << std::dec << " operations=" << operations
              << " reason=" << run.error() << '\n';
    run.trace().print(std::cerr);
    std::cerr << "reproduce: test_order_book_soak --operations " << operations
              << " --seed 0x" << std::hex << seed << std::dec << " --scenario "
              << scenario.name << '\n';
    return false;
  }
  const double elapsed =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - started)
          .count();
  const MemoryUsage memory = memory_usage();
  std::cout << "PASS scenario=" << scenario.name << " seed=0x" << std::hex
            << seed << std::dec << " operations=" << operations
            << " elapsed_s=" << std::fixed << std::setprecision(3) << elapsed
            << " throughput_ops_s="
            << static_cast<std::uint64_t>(operations /
                                          (std::max)(elapsed, 0.001))
            << " rss_bytes=" << memory.resident_bytes
            << " peak_rss_bytes=" << memory.peak_resident_bytes << '\n';
  return true;
}

} // namespace

int main(int argc, char* argv[]) {
  try {
    const Options options = parse_options(argc, argv);
    std::vector<const Scenario*> selected_scenarios;
    selected_scenarios.reserve(scenarios.size());
    for (const Scenario& scenario : scenarios) {
      if (options.scenario_name.empty() ||
          options.scenario_name == scenario.name) {
        selected_scenarios.push_back(&scenario);
      }
    }
    if (selected_scenarios.empty()) {
      throw std::invalid_argument("unknown scenario: " + options.scenario_name);
    }
    std::cout << "order_book_soak mode=" << mode_name(options.mode)
              << " requested_operations=" << options.operations
              << " validation_interval=" << options.validation_interval
              << " seeds=" << options.seeds.size()
              << " scenarios=" << selected_scenarios.size()
              << " sizeof_size_t=" << sizeof(std::size_t)
#ifdef _MSC_VER
              << " compiler=MSVC-" << _MSC_VER
#elif defined(__clang__)
              << " compiler=Clang-" << __clang_major__ << '.' << __clang_minor__
#elif defined(__GNUC__)
              << " compiler=GCC-" << __GNUC__ << '.' << __GNUC_MINOR__
#endif
#ifdef NDEBUG
              << " build=Release"
#else
              << " build=Debug"
#endif
              << '\n';

    if (!failure_atomicity_tests() || !aggregate_quantity_test() ||
        !allocation_test() || !lifecycle_tests()) {
      std::cerr << "FAIL targeted preflight\n";
      return 2;
    }
    std::cout << "PASS targeted atomicity/allocation/aggregate/lifecycle\n";

    const std::uint64_t run_count = static_cast<std::uint64_t>(
        selected_scenarios.size() * options.seeds.size());
    const std::uint64_t base_operations = options.operations / run_count;
    std::uint64_t remainder = options.operations % run_count;
    std::uint64_t completed_operations = 0;
    const auto all_started = std::chrono::steady_clock::now();

    for (const std::uint64_t seed : options.seeds) {
      for (const Scenario* scenario : selected_scenarios) {
        const std::uint64_t operations =
            base_operations + (remainder != 0 ? 1U : 0U);
        if (remainder != 0) {
          --remainder;
        }
        if (!run_scenario(*scenario, seed, operations,
                          options.validation_interval)) {
          return 3;
        }
        completed_operations += operations;
      }
    }

    if (options.duration_seconds != 0) {
      const auto deadline = std::chrono::steady_clock::now() +
                            std::chrono::seconds(options.duration_seconds);
      std::uint64_t extension_seed =
          options.seeds.front() ^ 0x5A5A5A5A5A5A5A5AULL;
      while (std::chrono::steady_clock::now() < deadline) {
        constexpr std::uint64_t extension_operations = 250'000;
        if (!run_scenario(scenarios[8], extension_seed++, extension_operations,
                          options.validation_interval)) {
          return 4;
        }
        completed_operations += extension_operations;
      }
    }

    const double elapsed = std::chrono::duration<double>(
                               std::chrono::steady_clock::now() - all_started)
                               .count();
    const MemoryUsage memory = memory_usage();
    std::cout << "SUMMARY status=PASS total_operations=" << completed_operations
              << " elapsed_s=" << std::fixed << std::setprecision(3) << elapsed
              << " throughput_ops_s="
              << static_cast<std::uint64_t>(completed_operations /
                                            (std::max)(elapsed, 0.001))
              << " rss_bytes=" << memory.resident_bytes
              << " peak_rss_bytes=" << memory.peak_resident_bytes << '\n';
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "ERROR " << error.what() << '\n';
    return 1;
  }
}
