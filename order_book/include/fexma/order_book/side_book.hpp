#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

#include <fexma/order_book/order_pool.hpp>
#include <fexma/order_book/price_segment.hpp>

namespace fexma::order_book {

template <Side BookSide>
class SideBook {
public:
  SideBook() = default;

  SideBook(PriceTick min_price_tick, PriceTick max_price_tick)
      : min_price_tick_(min_price_tick),
        max_price_tick_(max_price_tick),
        base_segment_(min_price_tick >> price_segment_shift),
        segment_count_(segment_count_for(min_price_tick, max_price_tick)),
        segments_(segment_count_ == 0
                      ? nullptr
                      : std::make_unique<PriceSegment[]>(segment_count_)) {
    clear();
  }

  SideBook(const SideBook&) = delete;
  SideBook& operator=(const SideBook&) = delete;
  SideBook(SideBook&&) noexcept = default;
  SideBook& operator=(SideBook&&) noexcept = default;

  void warm_up(std::uint32_t page_size = 4096) noexcept {
    touch_pages(segments_.get(), sizeof(PriceSegment) * segment_count_,
                page_size);
    clear();
  }

  void clear() noexcept {
    best_segment_ = invalid_segment();
    order_count_ = 0;
    total_quantity_ = 0;
    for (std::size_t i = 0; i < segment_count_; ++i) {
      segments_[i] = PriceSegment{};
    }
  }

  [[nodiscard]] bool contains_price(PriceTick price) const noexcept {
    return price >= min_price_tick_ && price <= max_price_tick_;
  }

  void append(OrderPool& pool, OrderSlot slot) noexcept {
    Order& order = pool[slot];
    const std::size_t segment = local_segment(order.price);
    const std::uint32_t offset = price_offset(order.price);
    PriceSegment& price_segment = segments_[segment];
    PriceLevel& level = price_segment.levels[offset];

    order.prev = level.tail;
    order.next = invalid_order_slot;
    if (level.tail != invalid_order_slot) {
      pool[level.tail].next = slot;
    } else {
      level.head = slot;
      price_segment.active_mask |= (std::uint64_t{1} << offset);
    }
    level.tail = slot;
    level.total_quantity += order.remaining;
    ++level.order_count;
    ++order_count_;
    total_quantity_ += order.remaining;
    update_best_after_insert(segment);
  }

  void remove(OrderPool& pool, OrderSlot slot) noexcept {
    Order& order = pool[slot];
    const std::size_t segment = local_segment(order.price);
    const std::uint32_t offset = price_offset(order.price);
    PriceSegment& price_segment = segments_[segment];
    PriceLevel& level = price_segment.levels[offset];

    if (order.prev != invalid_order_slot) {
      pool[order.prev].next = order.next;
    } else {
      level.head = order.next;
    }

    if (order.next != invalid_order_slot) {
      pool[order.next].prev = order.prev;
    } else {
      level.tail = order.prev;
    }

    level.total_quantity -= order.remaining;
    --level.order_count;
    --order_count_;
    total_quantity_ -= order.remaining;
    order.prev = invalid_order_slot;
    order.next = invalid_order_slot;

    if (level.order_count == 0) {
      price_segment.active_mask &= ~(std::uint64_t{1} << offset);
      if (segment == best_segment_ && price_segment.empty()) {
        recompute_best();
      }
    }
  }

  void reduce(OrderPool& pool, OrderSlot slot, Quantity quantity) noexcept {
    Order& order = pool[slot];
    const std::size_t segment = local_segment(order.price);
    const std::uint32_t offset = price_offset(order.price);
    PriceLevel& level = segments_[segment].levels[offset];
    order.remaining -= quantity;
    level.total_quantity -= quantity;
    total_quantity_ -= quantity;
  }

  [[nodiscard]] OrderSlot best_order(const OrderPool& pool) const noexcept {
    const PriceLevel* level = best_level();
    if (level == nullptr) {
      return invalid_order_slot;
    }
    (void)pool;
    return level->head;
  }

  [[nodiscard]] PriceTick best_price() const noexcept {
    if (best_segment_ == invalid_segment()) {
      return {};
    }
    const PriceSegment& segment = segments_[best_segment_];
    const std::uint32_t offset = BookSide == Side::Ask
                                     ? segment.best_ask_offset()
                                     : segment.best_bid_offset();
    return price_from_local(best_segment_, offset);
  }

  [[nodiscard]] bool empty() const noexcept {
    return order_count_ == 0;
  }

  [[nodiscard]] std::uint32_t order_count() const noexcept {
    return order_count_;
  }

  [[nodiscard]] Quantity total_quantity() const noexcept {
    return total_quantity_;
  }

  [[nodiscard]] std::size_t segment_count() const noexcept {
    return segment_count_;
  }

  [[nodiscard]] const PriceSegment& segment(std::size_t index) const noexcept {
    return segments_[index];
  }

  [[nodiscard]] PriceTick price_from_local(std::size_t segment,
                                           std::uint32_t offset) const noexcept {
    return static_cast<PriceTick>(((base_segment_ + segment)
                                  << price_segment_shift) + offset);
  }

  [[nodiscard]] std::size_t local_segment(PriceTick price) const noexcept {
    return (price >> price_segment_shift) - base_segment_;
  }

  [[nodiscard]] std::uint32_t price_offset(PriceTick price) const noexcept {
    return price & price_offset_mask;
  }

  [[nodiscard]] std::size_t best_segment() const noexcept {
    return best_segment_;
  }

  void recompute_best() noexcept {
    best_segment_ = invalid_segment();
    if constexpr (BookSide == Side::Ask) {
      for (std::size_t i = 0; i < segment_count_; ++i) {
        if (!segments_[i].empty()) {
          best_segment_ = i;
          return;
        }
      }
    } else {
      for (std::size_t i = segment_count_; i > 0; --i) {
        if (!segments_[i - 1].empty()) {
          best_segment_ = i - 1;
          return;
        }
      }
    }
  }

private:
  [[nodiscard]] const PriceLevel* best_level() const noexcept {
    if (best_segment_ == invalid_segment()) {
      return nullptr;
    }
    const PriceSegment& segment = segments_[best_segment_];
    const std::uint32_t offset = BookSide == Side::Ask
                                     ? segment.best_ask_offset()
                                     : segment.best_bid_offset();
    return &segment.levels[offset];
  }

  void update_best_after_insert(std::size_t segment) noexcept {
    if (best_segment_ == invalid_segment()) {
      best_segment_ = segment;
      return;
    }

    if constexpr (BookSide == Side::Ask) {
      if (segment < best_segment_) {
        best_segment_ = segment;
      }
    } else {
      if (segment > best_segment_) {
        best_segment_ = segment;
      }
    }
  }

  [[nodiscard]] static constexpr std::size_t invalid_segment() noexcept {
    return static_cast<std::size_t>(-1);
  }

  static std::size_t segment_count_for(PriceTick min_price_tick,
                                       PriceTick max_price_tick) noexcept {
    if (max_price_tick < min_price_tick) {
      return 0;
    }
    const std::size_t first = min_price_tick >> price_segment_shift;
    const std::size_t last = max_price_tick >> price_segment_shift;
    return last - first + 1;
  }

  static void touch_pages(void* memory, std::size_t bytes,
                          std::uint32_t page_size) noexcept {
    if (memory == nullptr || bytes == 0) {
      return;
    }
    const std::size_t step = page_size == 0 ? 4096U : page_size;
    auto* raw = static_cast<volatile std::uint8_t*>(memory);
    for (std::size_t offset = 0; offset < bytes; offset += step) {
      raw[offset] = raw[offset];
    }
    raw[bytes - 1] = raw[bytes - 1];
  }

  PriceTick min_price_tick_{};
  PriceTick max_price_tick_{};
  std::size_t base_segment_{};
  std::size_t segment_count_{};
  std::unique_ptr<PriceSegment[]> segments_;
  std::size_t best_segment_{invalid_segment()};
  std::uint32_t order_count_{};
  Quantity total_quantity_{};
};

} // namespace fexma::order_book
