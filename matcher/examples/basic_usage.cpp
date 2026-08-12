/**
 * @file basic_usage.cpp
 * @brief Minimal use of the matcher sample.
 */
#include <fexma/matcher/matcher.hpp>

#include <array>
#include <cstddef>
#include <iostream>

using namespace fexma::matcher;

namespace {

class ArrayCommandReader {
public:
  explicit ArrayCommandReader(const std::array<Command, 3>& commands)
      : commands_(commands) {}

  [[nodiscard]] CommandReadResult read_next() noexcept {
    if (next_ == commands_.size()) {
      return {CommandReadStatus::Shutdown, {}};
    }
    return {CommandReadStatus::Ok, commands_[next_++]};
  }

private:
  const std::array<Command, 3>& commands_;
  std::size_t next_{};
};

class CountingEventWriter {
public:
  [[nodiscard]] PublishResult publish(const Event& event) noexcept {
    if (event.type == EventType::Trade) {
      ++trades;
    }
    if (event.type == EventType::OrderRested) {
      ++rested;
    }
    if (event.type == EventType::OrderDone) {
      ++done;
    }
    return {PublishStatus::Ok};
  }

  std::uint32_t trades{};
  std::uint32_t rested{};
  std::uint32_t done{};
};

[[nodiscard]] Command new_limit(OrderId id, OwnerId owner_id, Side side,
                                PriceTick price, Quantity quantity) noexcept {
  return {CommandType::NewLimit, {id, owner_id, side, price, quantity}};
}

} // namespace

int main() {
  const std::array commands{
      new_limit(1, 101, Side::Ask, 105, 7),
      new_limit(2, 202, Side::Bid, 104, 3),
      new_limit(3, 303, Side::Bid, 106, 5),
  };

  ArrayCommandReader reader(commands);
  CountingEventWriter writer;
  Matcher matcher(reader, writer, OrderBookConfig{100, 200, 8});

  const RunResult result = matcher.run();
  if (!result.ok() || !matcher.book().validate_invariants()) {
    std::cerr << "matcher failed\n";
    return 1;
  }

  std::cout << "trades=" << writer.trades << " rested=" << writer.rested
            << " done=" << writer.done << '\n';
  return 0;
}
