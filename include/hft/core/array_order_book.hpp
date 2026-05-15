// include/hft/core/array_order_book.hpp
#pragma once

#include "hft/core/types.hpp"
#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <unordered_map>
#include <vector>

namespace hft::core {

// Order book backed by a flat price-indexed array.
//
// Compared to the std::map-based OrderBook:
//   Level lookup : O(1) direct array index      (vs O(log N) tree traversal)
//   Best bid/ask : O(1) maintained integer       (vs O(log N) map::begin)
//   Per-level FIFO: contiguous vector + head ptr (vs pointer-chained std::list)
//   Compaction of consumed/cancelled slots is amortised O(1) per order.
//
// Trade-off: requires knowing the price range at construction.
//   min_price – lowest supported price (scaled integer, inclusive)
//   num_ticks – number of distinct price points  (array length)
//   tick      – spacing between adjacent prices  (default 1)
class ArrayOrderBook {
public:
  using TradeFn = std::function<void(OrderId maker, OrderId taker, Price, Qty)>;

  ArrayOrderBook(Price min_price, uint32_t num_ticks, Price tick = 1)
      : min_price_(min_price), tick_(tick), num_ticks_(num_ticks),
        bids_(num_ticks), asks_(num_ticks) {}

  void set_trade_callback(TradeFn fn) { on_trade_ = std::move(fn); }

  // Returns false if o.price is outside [min_price_, min_price_ + num_ticks_*tick_).
  bool add_limit(Order o);
  bool cancel(OrderId id);
  bool reduce(OrderId id, Qty new_qty) noexcept;
  bool execute(OrderId id, Qty exec_qty) noexcept;

  std::optional<Price> best_bid() const noexcept {
    if (best_bid_slot_ < 0)
      return std::nullopt;
    return slot_to_price(static_cast<uint32_t>(best_bid_slot_));
  }
  std::optional<Price> best_ask() const noexcept {
    if (best_ask_slot_ < 0)
      return std::nullopt;
    return slot_to_price(static_cast<uint32_t>(best_ask_slot_));
  }

  Qty qty_at(Side s, Price price) const noexcept;
  std::size_t num_orders() const noexcept { return id_index_.size(); }
  std::size_t num_bid_levels() const noexcept { return num_bid_levels_; }
  std::size_t num_ask_levels() const noexcept { return num_ask_levels_; }

private:
  // One slot per price point on each side.
  // Cancelled orders are tombstoned (qty=0); head advances past consumed
  // front entries. Compaction runs when waste exceeds a threshold.
  struct Slot {
    Qty total_qty{0};
    uint32_t head{0};
    std::vector<Order> queue;

    bool empty() const noexcept { return total_qty == 0; }
  };

  // Points directly to an order in O(1) without any tree traversal.
  struct Locator {
    Side side;
    uint32_t slot;
    uint32_t queue_idx;
  };

  Price min_price_;
  Price tick_;
  uint32_t num_ticks_;

  std::vector<Slot> bids_; // bids_[slot], higher slot = higher price
  std::vector<Slot> asks_; // asks_[slot], lower  slot = lower  price

  // -1 means empty.
  int32_t best_bid_slot_{-1}; // highest occupied bid slot
  int32_t best_ask_slot_{-1}; // lowest  occupied ask slot

  std::size_t num_bid_levels_{0};
  std::size_t num_ask_levels_{0};

  std::unordered_map<OrderId, Locator> id_index_;
  TradeFn on_trade_;

  uint32_t price_to_slot(Price p) const noexcept {
    return static_cast<uint32_t>((p - min_price_) / tick_);
  }
  Price slot_to_price(uint32_t s) const noexcept {
    return min_price_ + static_cast<Price>(s) * tick_;
  }

  // Remove the consumed prefix of sl.queue when waste exceeds threshold.
  // Updates queue_idx in id_index_ for all surviving orders.
  void maybe_compact(Slot &sl) {
    while (sl.head < sl.queue.size() && sl.queue[sl.head].qty == 0)
      ++sl.head;

    if (sl.head < 32 || sl.head * 2 < static_cast<uint32_t>(sl.queue.size()))
      return;

    const uint32_t old_head = sl.head;
    sl.queue.erase(sl.queue.begin(), sl.queue.begin() + old_head);
    sl.head = 0;

    for (uint32_t i = 0; i < static_cast<uint32_t>(sl.queue.size()); ++i) {
      if (sl.queue[i].qty > 0) {
        auto it = id_index_.find(sl.queue[i].id);
        if (it != id_index_.end())
          it->second.queue_idx = i;
      }
    }
  }

  // Scan downward from start for the next non-empty bid slot.
  void rescan_best_bid(int32_t start) {
    for (int32_t s = start; s >= 0; --s) {
      if (!bids_[static_cast<uint32_t>(s)].empty()) {
        best_bid_slot_ = s;
        return;
      }
    }
    best_bid_slot_ = -1;
  }

  // Scan upward from start for the next non-empty ask slot.
  void rescan_best_ask(int32_t start) {
    for (int32_t s = start; s < static_cast<int32_t>(num_ticks_); ++s) {
      if (!asks_[static_cast<uint32_t>(s)].empty()) {
        best_ask_slot_ = s;
        return;
      }
    }
    best_ask_slot_ = -1;
  }

  void rest_(Order o);

  void match_buy_(Order &taker) {
    while (taker.qty > 0 && best_ask_slot_ >= 0) {
      const uint32_t sidx = static_cast<uint32_t>(best_ask_slot_);
      if (taker.price < slot_to_price(sidx))
        break;

      Slot &sl = asks_[sidx];
      while (taker.qty > 0 &&
             sl.head < static_cast<uint32_t>(sl.queue.size())) {
        Order &maker = sl.queue[sl.head];
        if (maker.qty == 0) {
          ++sl.head;
          continue;
        }

        const Qty traded = std::min(taker.qty, maker.qty);
        if (on_trade_)
          on_trade_(maker.id, taker.id, slot_to_price(sidx), traded);

        taker.qty -= traded;
        maker.qty -= traded;
        sl.total_qty -= traded;

        if (maker.qty == 0) {
          id_index_.erase(maker.id);
          ++sl.head;
        }
      }

      if (sl.empty()) {
        --num_ask_levels_;
        rescan_best_ask(best_ask_slot_ + 1);
      } else {
        maybe_compact(sl);
        break;
      }
    }
  }

  void match_sell_(Order &taker) {
    while (taker.qty > 0 && best_bid_slot_ >= 0) {
      const uint32_t sidx = static_cast<uint32_t>(best_bid_slot_);
      if (taker.price > slot_to_price(sidx))
        break;

      Slot &sl = bids_[sidx];
      while (taker.qty > 0 &&
             sl.head < static_cast<uint32_t>(sl.queue.size())) {
        Order &maker = sl.queue[sl.head];
        if (maker.qty == 0) {
          ++sl.head;
          continue;
        }

        const Qty traded = std::min(taker.qty, maker.qty);
        if (on_trade_)
          on_trade_(maker.id, taker.id, slot_to_price(sidx), traded);

        taker.qty -= traded;
        maker.qty -= traded;
        sl.total_qty -= traded;

        if (maker.qty == 0) {
          id_index_.erase(maker.id);
          ++sl.head;
        }
      }

      if (sl.empty()) {
        --num_bid_levels_;
        rescan_best_bid(best_bid_slot_ - 1);
      } else {
        maybe_compact(sl);
        break;
      }
    }
  }
};

inline void ArrayOrderBook::rest_(Order o) {
  const uint32_t sidx = price_to_slot(o.price);

  if (o.side == Side::Buy) {
    Slot &sl = bids_[sidx];
    const bool was_empty = sl.empty();
    const uint32_t qidx = static_cast<uint32_t>(sl.queue.size());
    sl.total_qty += o.qty;
    sl.queue.push_back(o);
    id_index_.emplace(o.id, Locator{Side::Buy, sidx, qidx});
    if (was_empty) {
      ++num_bid_levels_;
      if (best_bid_slot_ < static_cast<int32_t>(sidx))
        best_bid_slot_ = static_cast<int32_t>(sidx);
    }
  } else {
    Slot &sl = asks_[sidx];
    const bool was_empty = sl.empty();
    const uint32_t qidx = static_cast<uint32_t>(sl.queue.size());
    sl.total_qty += o.qty;
    sl.queue.push_back(o);
    id_index_.emplace(o.id, Locator{Side::Sell, sidx, qidx});
    if (was_empty) {
      ++num_ask_levels_;
      if (best_ask_slot_ < 0 || static_cast<int32_t>(sidx) < best_ask_slot_)
        best_ask_slot_ = static_cast<int32_t>(sidx);
    }
  }
}

inline bool ArrayOrderBook::add_limit(Order o) {
  if (price_to_slot(o.price) >= num_ticks_)
    return false;
  if (o.side == Side::Buy)
    match_buy_(o);
  else
    match_sell_(o);
  if (o.qty > 0)
    rest_(std::move(o));
  return true;
}

inline bool ArrayOrderBook::cancel(OrderId id) {
  auto it = id_index_.find(id);
  if (it == id_index_.end())
    return false;

  const Locator loc = it->second;
  id_index_.erase(it);

  Slot &sl = (loc.side == Side::Buy) ? bids_[loc.slot] : asks_[loc.slot];
  Order &ord = sl.queue[loc.queue_idx];
  sl.total_qty -= ord.qty;
  ord.qty = 0; // tombstone

  if (sl.empty()) {
    if (loc.side == Side::Buy) {
      --num_bid_levels_;
      if (best_bid_slot_ == static_cast<int32_t>(loc.slot))
        rescan_best_bid(best_bid_slot_ - 1);
    } else {
      --num_ask_levels_;
      if (best_ask_slot_ == static_cast<int32_t>(loc.slot))
        rescan_best_ask(best_ask_slot_ + 1);
    }
  }
  return true;
}

inline bool ArrayOrderBook::reduce(OrderId id, Qty new_qty) noexcept {
  auto it = id_index_.find(id);
  if (it == id_index_.end())
    return false;

  const Locator &loc = it->second;
  Slot &sl = (loc.side == Side::Buy) ? bids_[loc.slot] : asks_[loc.slot];
  Order &ord = sl.queue[loc.queue_idx];

  if (new_qty == 0 || new_qty >= ord.qty)
    return false;

  sl.total_qty -= (ord.qty - new_qty);
  ord.qty = new_qty;
  return true;
}

inline bool ArrayOrderBook::execute(OrderId id, Qty exec_qty) noexcept {
  auto it = id_index_.find(id);
  if (it == id_index_.end())
    return false;

  const Locator loc = it->second;
  Slot &sl = (loc.side == Side::Buy) ? bids_[loc.slot] : asks_[loc.slot];
  Order &ord = sl.queue[loc.queue_idx];

  if (exec_qty == 0 || exec_qty > ord.qty)
    return false;

  ord.qty -= exec_qty;
  sl.total_qty -= exec_qty;

  if (ord.qty == 0) {
    id_index_.erase(it);
    if (sl.empty()) {
      if (loc.side == Side::Buy) {
        --num_bid_levels_;
        if (best_bid_slot_ == static_cast<int32_t>(loc.slot))
          rescan_best_bid(best_bid_slot_ - 1);
      } else {
        --num_ask_levels_;
        if (best_ask_slot_ == static_cast<int32_t>(loc.slot))
          rescan_best_ask(best_ask_slot_ + 1);
      }
    }
  }
  return true;
}

inline Qty ArrayOrderBook::qty_at(Side s, Price price) const noexcept {
  const uint32_t sidx = price_to_slot(price);
  if (sidx >= num_ticks_)
    return 0;
  const Slot &sl = (s == Side::Buy) ? bids_[sidx] : asks_[sidx];
  return sl.total_qty;
}

} // namespace hft::core
