// include/hft/core/array_order_book.hpp
#pragma once

#include "hft/core/null_listener.hpp"
#include "hft/core/types.hpp"
#include "hft/md/md_event.hpp"
#include <algorithm>
#include <cassert>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <unordered_map>
#include <vector>

namespace hft::core {

// ─── ArrayOrderBook ───────────────────────────────────────────────────
//
// Order book backed by a flat price-indexed array.
//
// Compared to the std::map-based OrderBook:
//   Level lookup : O(1) direct array index      (vs O(log N) tree traversal)
//   Best bid/ask : O(1) maintained integer       (vs O(log N) map::begin)
//   Per-level FIFO: contiguous vector + head ptr (vs pointer-chained std::list)
//   Compaction of consumed/cancelled slots is amortised O(1) per order.
//
// Trade-off: requires knowing the price range at construction.
//
// Listener contract (template — no virtual, compiler can inline the whole chain):
//   void on_bbo(SymbolId, Price bid, Qty bid_qty, Price ask, Qty ask_qty) noexcept
//   template<class Book> void on_depth(SymbolId, const Book&) noexcept
//   void on_trade(SymbolId, Price, Qty, TradeId) noexcept
//
// Dispatch rules (mutually exclusive per event):
//   BBO changed  → on_bbo only
//   BBO stable   → on_depth only
//   Trade event  → on_trade (in addition to whichever book callback fires)
//
template <typename Listener = detail::NullListener>
class ArrayOrderBook {
public:
  using TradeFn = std::function<void(OrderId maker, OrderId taker, Price, Qty)>;

  // BBO snapshot: 0 on a side means that side is empty.
  struct Bbo {
    Price bid{0};
    Qty   bid_qty{0};
    Price ask{0};
    Qty   ask_qty{0};
    bool operator==(const Bbo &) const noexcept = default;
  };

  // ── Constructors ──────────────────────────────────────────────────

  // Strategy/production constructor: listener is called on every book event.
  ArrayOrderBook(Listener &l, md::SymbolId sym,
                 Price min_price, uint32_t num_ticks, Price tick = 1)
      : listener_(&l), sym_(sym),
        min_price_(min_price), tick_(tick), num_ticks_(num_ticks),
        bids_(num_ticks), asks_(num_ticks) {}

  // Standalone/test constructor: no listener required.
  // Only available when Listener is the NullListener default.
  ArrayOrderBook(Price min_price, uint32_t num_ticks, Price tick = 1)
      requires std::same_as<Listener, detail::NullListener>
      : listener_(&detail::g_null_listener), sym_(0),
        min_price_(min_price), tick_(tick), num_ticks_(num_ticks),
        bids_(num_ticks), asks_(num_ticks) {}

  // ── Trade callback (used by the standalone matching engine path) ──
  void set_trade_callback(TradeFn fn) { on_trade_ = std::move(fn); }

  // ── Core order operations ─────────────────────────────────────────

  // Returns false if o.price is outside [min_price_, min_price_ + num_ticks_*tick_).
  bool add_limit(Order o);
  bool cancel(OrderId id);
  bool reduce(OrderId id, Qty new_qty) noexcept;
  bool execute(OrderId id, Qty exec_qty) noexcept;

  // ── Market-data apply interface ──────────────────────────────────
  // Mutate the book from a feed event and fire listener callbacks.
  // apply(Add) rests the order without internal crossing (MD path).

  void apply(const md::Add &a);
  void apply(const md::Cancel &c);
  void apply(const md::Reduce &r);
  void apply(const md::Exec &x);
  void apply(const md::Replace &r);
  void apply(const md::Trade &t);
  void apply(const md::Clear &);

  // ── Queries ──────────────────────────────────────────────────────

  std::optional<Price> best_bid() const noexcept {
    if (best_bid_slot_ < 0) return std::nullopt;
    return slot_to_price(static_cast<uint32_t>(best_bid_slot_));
  }
  std::optional<Price> best_ask() const noexcept {
    if (best_ask_slot_ < 0) return std::nullopt;
    return slot_to_price(static_cast<uint32_t>(best_ask_slot_));
  }

  Qty qty_at(Side s, Price price) const noexcept;
  std::size_t num_orders() const noexcept { return id_index_.size(); }
  std::size_t num_bid_levels() const noexcept { return num_bid_levels_; }
  std::size_t num_ask_levels() const noexcept { return num_ask_levels_; }

  Bbo snapshot_bbo() const noexcept {
    Bbo b;
    if (best_bid_slot_ >= 0) {
      const uint32_t s = static_cast<uint32_t>(best_bid_slot_);
      b.bid     = slot_to_price(s);
      b.bid_qty = bids_[s].total_qty;
    }
    if (best_ask_slot_ >= 0) {
      const uint32_t s = static_cast<uint32_t>(best_ask_slot_);
      b.ask     = slot_to_price(s);
      b.ask_qty = asks_[s].total_qty;
    }
    return b;
  }

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

  Listener *listener_;
  md::SymbolId sym_;

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
  TradeFn on_trade_; // optional matching-engine callback

  uint32_t price_to_slot(Price p) const noexcept {
    return static_cast<uint32_t>((p - min_price_) / tick_);
  }
  Price slot_to_price(uint32_t s) const noexcept {
    return min_price_ + static_cast<Price>(s) * tick_;
  }

  // Fire on_bbo or on_depth depending on whether the BBO moved.
  void notify_(const Bbo &before) noexcept {
    const Bbo after = snapshot_bbo();
    if (after != before)
      listener_->on_bbo(sym_, after.bid, after.bid_qty, after.ask, after.ask_qty);
    else
      listener_->on_depth(sym_, *this);
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

// ─── rest_ ────────────────────────────────────────────────────────────

template <typename Listener>
inline void ArrayOrderBook<Listener>::rest_(Order o) {
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

// ─── add_limit ────────────────────────────────────────────────────────

template <typename Listener>
inline bool ArrayOrderBook<Listener>::add_limit(Order o) {
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

// ─── cancel ───────────────────────────────────────────────────────────

template <typename Listener>
inline bool ArrayOrderBook<Listener>::cancel(OrderId id) {
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

// ─── reduce ───────────────────────────────────────────────────────────

template <typename Listener>
inline bool ArrayOrderBook<Listener>::reduce(OrderId id, Qty new_qty) noexcept {
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

// ─── execute ──────────────────────────────────────────────────────────

template <typename Listener>
inline bool ArrayOrderBook<Listener>::execute(OrderId id, Qty exec_qty) noexcept {
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

// ─── qty_at ───────────────────────────────────────────────────────────

template <typename Listener>
inline Qty ArrayOrderBook<Listener>::qty_at(Side s, Price price) const noexcept {
  const uint32_t sidx = price_to_slot(price);
  if (sidx >= num_ticks_)
    return 0;
  const Slot &sl = (s == Side::Buy) ? bids_[sidx] : asks_[sidx];
  return sl.total_qty;
}

// ─── apply (market-data interface) ────────────────────────────────────

template <typename Listener>
inline void ArrayOrderBook<Listener>::apply(const md::Add &a) {
  if (price_to_slot(a.px) >= num_ticks_) return;
  const Bbo before = snapshot_bbo();
  Order o{};
  o.id = a.id; o.side = a.side; o.price = a.px; o.qty = a.qty; o.ts = a.ts;
  rest_(o);
  notify_(before);
}

template <typename Listener>
inline void ArrayOrderBook<Listener>::apply(const md::Cancel &c) {
  const Bbo before = snapshot_bbo();
  if (cancel(c.id))
    notify_(before);
}

template <typename Listener>
inline void ArrayOrderBook<Listener>::apply(const md::Reduce &r) {
  const Bbo before = snapshot_bbo();
  if (reduce(r.id, r.new_qty))
    notify_(before);
}

template <typename Listener>
inline void ArrayOrderBook<Listener>::apply(const md::Exec &x) {
  const Bbo before = snapshot_bbo();
  if (execute(x.id, x.exec_qty)) {
    listener_->on_trade(sym_, x.px, x.exec_qty, x.trade_id);
    notify_(before);
  }
}

template <typename Listener>
inline void ArrayOrderBook<Listener>::apply(const md::Replace &r) {
  if (price_to_slot(r.px) >= num_ticks_) return;
  const Bbo before = snapshot_bbo();
  cancel(r.old_id);
  Order o{};
  o.id = r.new_id; o.side = r.side; o.price = r.px; o.qty = r.qty; o.ts = r.ts;
  rest_(o);
  notify_(before);
}

template <typename Listener>
inline void ArrayOrderBook<Listener>::apply(const md::Trade &t) {
  listener_->on_trade(sym_, t.px, t.qty, t.trade_id);
}

template <typename Listener>
inline void ArrayOrderBook<Listener>::apply(const md::Clear &) {
  const Bbo before = snapshot_bbo();
  bids_.assign(num_ticks_, Slot{});
  asks_.assign(num_ticks_, Slot{});
  best_bid_slot_ = -1;
  best_ask_slot_ = -1;
  num_bid_levels_ = 0;
  num_ask_levels_ = 0;
  id_index_.clear();
  notify_(before);
}

} // namespace hft::core
