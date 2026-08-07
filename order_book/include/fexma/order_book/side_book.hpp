/**
 * @file side_book.hpp
 * @brief One-sided price-indexed book for bids or asks.
 *
 * SideBook owns a fixed array of price segments for a configured price range.
 * It maintains FIFO levels, side aggregate counts, and a cached best segment.
 */
#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <memory>

#include <fexma/order_book/detail/order_pool.hpp>
#include <fexma/order_book/price_segment.hpp>

namespace fexma::order_book {

/**
 * @brief Bid or ask storage selected at compile time.
 *
 * @tparam BookSide Side::Bid finds maximum prices; Side::Ask finds minimum
 * prices. The template avoids runtime side branches inside best-price scans.
 */
template <Side BookSide>
class SideBook {
public:
  SideBook() = default;

  SideBook(PriceTick min_price_tick, PriceTick max_price_tick)
      : min_price_tick_(min_price_tick),
        max_price_tick_(max_price_tick),
        base_segment_(min_price_tick >> price_segment_shift),
        segment_count_(segment_count_for(min_price_tick, max_price_tick)),
        segment_occupancy_word_count_(word_count_for(segment_count_)),
        segments_(segment_count_ == 0
                      ? nullptr
                      : std::make_unique<PriceSegment[]>(segment_count_)),
        segment_occupancy_(segment_occupancy_word_count_ == 0
                               ? nullptr
                               : std::make_unique<std::uint64_t[]>(
                                     segment_occupancy_word_count_)) {
    clear();
  }

  SideBook(const SideBook&) = delete;
  SideBook& operator=(const SideBook&) = delete;
  SideBook(SideBook&&) noexcept = default;
  SideBook& operator=(SideBook&&) noexcept = default;

  void clear() noexcept {
    best_segment_ = invalid_segment();
    order_count_ = 0;
    total_quantity_ = 0;
    for (std::size_t i = 0; i < segment_count_; ++i) {
      segments_[i] = PriceSegment{};
    }
    for (std::size_t i = 0; i < segment_occupancy_word_count_; ++i) {
      segment_occupancy_[i] = 0;
    }
  }

  [[nodiscard]] bool contains_price(PriceTick price) const noexcept {
    return price >= min_price_tick_ && price <= max_price_tick_;
  }

  void append(detail::OrderPool& pool, OrderIndex slot) noexcept {
    detail::Order& order = pool.get_unchecked(slot);
    const std::size_t segment = local_segment(order.price);
    const std::uint32_t offset = price_offset(order.price);
    PriceSegment& price_segment = segments_[segment];
    PriceLevel& level = price_segment.levels[offset];
    const bool segment_was_empty = price_segment.empty();

    order.prev = level.tail;
    order.next = invalid_order_index;
    if (level.tail != invalid_order_index) {
      pool.get_unchecked(level.tail).next = slot;
    } else {
      level.head = slot;
      // The occupancy bit must be set exactly when the FIFO transitions from
      // empty to non-empty; validate_invariants() mirrors this relation.
      price_segment.active_mask |= (std::uint64_t{1} << offset);
    }
    if (segment_was_empty) {
      set_segment_occupied(segment);
    }
    level.tail = slot;
    level.total_quantity += order.remaining;
    ++level.order_count;
    ++order_count_;
    total_quantity_ += order.remaining;
    update_best_after_insert(segment);
  }

  void remove(detail::OrderPool& pool, OrderIndex slot) noexcept {
    detail::Order& order = pool.get_unchecked(slot);
    const std::size_t segment = local_segment(order.price);
    const std::uint32_t offset = price_offset(order.price);
    PriceSegment& price_segment = segments_[segment];
    PriceLevel& level = price_segment.levels[offset];

    // Unlink in O(1) using intrusive prev/next pool indices; this allows erase
    // by OrderId without walking the price level.
    if (order.prev != invalid_order_index) {
      pool.get_unchecked(order.prev).next = order.next;
    } else {
      level.head = order.next;
    }

    if (order.next != invalid_order_index) {
      pool.get_unchecked(order.next).prev = order.prev;
    } else {
      level.tail = order.prev;
    }

    level.total_quantity -= order.remaining;
    --level.order_count;
    --order_count_;
    total_quantity_ -= order.remaining;
    order.prev = invalid_order_index;
    order.next = invalid_order_index;

    if (level.order_count == 0) {
      // Clearing the bit is coupled with the last-order removal from a price.
      price_segment.active_mask &= ~(std::uint64_t{1} << offset);
      if (segment == best_segment_ && price_segment.empty()) {
        clear_segment_occupied(segment);
        // Only removal of the current best segment can require a best update.
        recompute_best();
      } else if (price_segment.empty()) {
        clear_segment_occupied(segment);
      }
    }
  }

  void reduce(detail::OrderPool& pool, OrderIndex slot,
              Quantity quantity) noexcept {
    detail::Order& order = pool.get_unchecked(slot);
    const std::size_t segment = local_segment(order.price);
    const std::uint32_t offset = price_offset(order.price);
    PriceLevel& level = segments_[segment].levels[offset];
    order.remaining -= quantity;
    level.total_quantity -= quantity;
    total_quantity_ -= quantity;
  }

  void set_remaining(detail::OrderPool& pool, OrderIndex slot,
                     Quantity new_remaining) noexcept {
    detail::Order& order = pool.get_unchecked(slot);
    const std::size_t segment = local_segment(order.price);
    const std::uint32_t offset = price_offset(order.price);
    PriceLevel& level = segments_[segment].levels[offset];
    if (new_remaining >= order.remaining) {
      const Quantity increase = new_remaining - order.remaining;
      level.total_quantity += increase;
      total_quantity_ += increase;
    } else {
      const Quantity decrease = order.remaining - new_remaining;
      level.total_quantity -= decrease;
      total_quantity_ -= decrease;
    }
    order.remaining = new_remaining;
  }

  [[nodiscard]] OrderIndex best_order(
      const detail::OrderPool& pool) const noexcept {
    const PriceLevel* level = best_level();
    if (level == nullptr) {
      return invalid_order_index;
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

  [[nodiscard]] std::size_t byte_size() const noexcept {
    return sizeof(PriceSegment) * segment_count_ +
           sizeof(std::uint64_t) * segment_occupancy_word_count_;
  }

  [[nodiscard]] std::size_t segment_occupancy_word_count() const noexcept {
    return segment_occupancy_word_count_;
  }

  [[nodiscard]] std::uint64_t
  segment_occupancy_word(std::size_t index) const noexcept {
    return segment_occupancy_[index];
  }

  void recompute_best() noexcept {
    best_segment_ = BookSide == Side::Ask ? first_occupied_segment()
                                          : last_occupied_segment();
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

  [[nodiscard]] static constexpr std::size_t
  word_count_for(std::size_t bit_count) noexcept {
    return (bit_count + 63U) / 64U;
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

  void set_segment_occupied(std::size_t segment) noexcept {
    segment_occupancy_[segment >> 6U] |=
        std::uint64_t{1} << (segment & 63U);
  }

  void clear_segment_occupied(std::size_t segment) noexcept {
    segment_occupancy_[segment >> 6U] &=
        ~(std::uint64_t{1} << (segment & 63U));
  }

  [[nodiscard]] std::size_t first_occupied_segment() const noexcept {
    for (std::size_t word_index = 0; word_index < segment_occupancy_word_count_;
         ++word_index) {
      const std::uint64_t word = segment_occupancy_[word_index];
      if (word != 0) {
        return word_index * 64U +
               static_cast<std::size_t>(std::countr_zero(word));
      }
    }
    return invalid_segment();
  }

  [[nodiscard]] std::size_t last_occupied_segment() const noexcept {
    for (std::size_t word_index = segment_occupancy_word_count_; word_index > 0;
         --word_index) {
      const std::uint64_t word = segment_occupancy_[word_index - 1U];
      if (word != 0) {
        return (word_index - 1U) * 64U + 63U -
               static_cast<std::size_t>(std::countl_zero(word));
      }
    }
    return invalid_segment();
  }

  PriceTick min_price_tick_{};
  PriceTick max_price_tick_{};
  std::size_t base_segment_{};
  std::size_t segment_count_{};
  std::size_t segment_occupancy_word_count_{};
  std::unique_ptr<PriceSegment[]> segments_;
  std::unique_ptr<std::uint64_t[]> segment_occupancy_;
  std::size_t best_segment_{invalid_segment()};
  std::uint32_t order_count_{};
  Quantity total_quantity_{};
};

} // namespace fexma::order_book
