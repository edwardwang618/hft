// include/hft/core/map_order_book.hpp
#pragma once

#include "hft/core/null_listener.hpp"
#include "hft/core/types.hpp"
#include "hft/md/md_event.hpp"

#include <concepts>
#include <cstddef>
#include <functional>
#include <list>
#include <map>
#include <optional>
#include <unordered_map>
#include <utility>

namespace hft::core {

// ─── A single price level: FIFO of orders at one price ────────────────
struct PriceLevel {
  Qty total_qty{0};
  std::list<Order> orders; // iterator-stable FIFO
};

// ─── MapOrderBook ─────────────────────────────────────────────────────
//
// Limit order book backed by std::map (red-black tree per side).
// Supports arbitrary price ranges with no pre-allocation.
//
// Trade-off vs ArrayOrderBook:
//   Level lookup : O(log N)  (vs O(1) array index)
//   Best bid/ask : O(log N)  (vs O(1) maintained integer)
//   Price range  : unbounded (vs fixed window)
//
// Listener contract (template — no virtual, compiler can inline the chain):
//   void on_bbo(SymbolId, Price bid, Qty bq, Price ask, Qty aq) noexcept
//   template<class Book> void on_depth(SymbolId, const Book&) noexcept
//   void on_trade(SymbolId, Price, Qty, TradeId) noexcept
//
// Dispatch rules (mutually exclusive per event):
//   BBO changed → on_bbo only
//   BBO stable  → on_depth only
//   Trade event → on_trade (plus whichever book callback fires)
//
template <typename Listener = detail::NullListener>
class MapOrderBook {
public:
  using TradeFn = std::function<void(OrderId maker, OrderId taker, Price, Qty)>;

  struct Bbo {
    Price bid{0};
    Qty   bid_qty{0};
    Price ask{0};
    Qty   ask_qty{0};
    bool operator==(const Bbo &) const noexcept = default;
  };

  // ── Constructors ─────────────────────────────────────────────────

  // Strategy/production constructor.
  MapOrderBook(Listener &l, md::SymbolId sym) : listener_(&l), sym_(sym) {}

  // Standalone/test constructor: only when Listener is NullListener.
  MapOrderBook()
      requires std::same_as<Listener, detail::NullListener>
      : listener_(&detail::g_null_listener), sym_(0) {}

  // ── Trade callback (standalone matching engine path) ─────────────
  void set_trade_callback(TradeFn fn) { on_trade_ = std::move(fn); }

  // ── Core order operations ─────────────────────────────────────────

  // Add a limit order. Crosses against the book first; residual rests.
  void add_limit(Order o);

  // Cancel by id. Returns false if id unknown.
  bool cancel(OrderId id);

  // Partial cancel: reduce resting qty to new_qty.
  // Requires 0 < new_qty < current qty. Returns false on invalid args.
  bool reduce(OrderId id, Qty new_qty) noexcept;

  // Passive execution: deduct exec_qty from resting order.
  // Returns false if id unknown or exec_qty > current qty.
  bool execute(OrderId id, Qty exec_qty) noexcept;

  // ── Market-data apply interface ──────────────────────────────────
  // Rests the order without internal crossing (tracking external state).

  void apply(const md::Add &a);
  void apply(const md::Cancel &c);
  void apply(const md::Reduce &r);
  void apply(const md::Exec &x);
  void apply(const md::Replace &r);
  void apply(const md::Trade &t);
  void apply(const md::Clear &);

  // ── Queries ──────────────────────────────────────────────────────

  std::optional<Price> best_bid() const {
    return bids_.empty() ? std::nullopt : std::optional{bids_.begin()->first};
  }
  std::optional<Price> best_ask() const {
    return asks_.empty() ? std::nullopt : std::optional{asks_.begin()->first};
  }

  Qty qty_at(Side s, Price price) const;
  std::size_t num_orders() const noexcept { return id_index_.size(); }
  std::size_t num_bid_levels() const noexcept { return bids_.size(); }
  std::size_t num_ask_levels() const noexcept { return asks_.size(); }

  Bbo snapshot_bbo() const noexcept {
    Bbo b;
    if (!bids_.empty()) {
      auto it = bids_.begin();
      b.bid     = it->first;
      b.bid_qty = it->second.total_qty;
    }
    if (!asks_.empty()) {
      auto it = asks_.begin();
      b.ask     = it->first;
      b.ask_qty = it->second.total_qty;
    }
    return b;
  }

private:
  // std::greater → bids_.begin() is the *highest* price (best bid)
  // std::less    → asks_.begin() is the *lowest*  price (best ask)
  using BidMap = std::map<Price, PriceLevel, std::greater<Price>>;
  using AskMap = std::map<Price, PriceLevel, std::less<Price>>;

  struct Locator {
    Side side;
    Price price;
    std::list<Order>::iterator it; // stable as long as list isn't erased
  };

  Listener *listener_;
  md::SymbolId sym_;

  BidMap bids_;
  AskMap asks_;
  std::unordered_map<OrderId, Locator> id_index_;
  TradeFn on_trade_;

  void notify_(const Bbo &before) noexcept {
    const Bbo after = snapshot_bbo();
    if (after != before)
      listener_->on_bbo(sym_, after.bid, after.bid_qty, after.ask, after.ask_qty);
    else
      listener_->on_depth(sym_, *this);
  }

  template <typename OppMap>
  void match_(OppMap &opp, Order &taker);

  void rest_(Order o);
};

// ─── match_ ───────────────────────────────────────────────────────────

template <typename Listener>
template <typename OppMap>
void MapOrderBook<Listener>::match_(OppMap &opp, Order &taker) {
  while (taker.qty > 0 && !opp.empty()) {
    auto lvl_it = opp.begin();
    const Price best = lvl_it->first;

    const bool crosses = (taker.side == Side::Buy) ? (taker.price >= best)
                                                    : (taker.price <= best);
    if (!crosses)
      break;

    auto &lvl = lvl_it->second;

    while (taker.qty > 0 && !lvl.orders.empty()) {
      Order &maker = lvl.orders.front();
      const Qty traded = (taker.qty < maker.qty) ? taker.qty : maker.qty;

      if (on_trade_)
        on_trade_(maker.id, taker.id, best, traded);

      taker.qty -= traded;
      maker.qty -= traded;
      lvl.total_qty -= traded;

      if (maker.qty == 0) {
        id_index_.erase(maker.id);
        lvl.orders.pop_front();
      }
    }
    if (lvl.orders.empty())
      opp.erase(lvl_it);
  }
}

// ─── rest_ ────────────────────────────────────────────────────────────

template <typename Listener>
inline void MapOrderBook<Listener>::rest_(Order o) {
  if (o.side == Side::Buy) {
    auto &lvl = bids_[o.price];
    lvl.orders.push_back(o);
    lvl.total_qty += o.qty;
    id_index_.emplace(o.id, Locator{Side::Buy, o.price, std::prev(lvl.orders.end())});
  } else {
    auto &lvl = asks_[o.price];
    lvl.orders.push_back(o);
    lvl.total_qty += o.qty;
    id_index_.emplace(o.id, Locator{Side::Sell, o.price, std::prev(lvl.orders.end())});
  }
}

// ─── add_limit ────────────────────────────────────────────────────────

template <typename Listener>
inline void MapOrderBook<Listener>::add_limit(Order o) {
  if (o.side == Side::Buy)
    match_(asks_, o);
  else
    match_(bids_, o);

  if (o.qty > 0)
    rest_(std::move(o));
}

// ─── cancel ───────────────────────────────────────────────────────────

template <typename Listener>
inline bool MapOrderBook<Listener>::cancel(OrderId id) {
  auto ix = id_index_.find(id);
  if (ix == id_index_.end())
    return false;

  const Locator loc = ix->second;
  id_index_.erase(ix);

  const Qty qty = loc.it->qty;
  if (loc.side == Side::Buy) {
    auto &lvl = bids_[loc.price];
    lvl.total_qty -= qty;
    lvl.orders.erase(loc.it);
    if (lvl.orders.empty())
      bids_.erase(loc.price);
  } else {
    auto &lvl = asks_[loc.price];
    lvl.total_qty -= qty;
    lvl.orders.erase(loc.it);
    if (lvl.orders.empty())
      asks_.erase(loc.price);
  }
  return true;
}

// ─── reduce ───────────────────────────────────────────────────────────

template <typename Listener>
inline bool MapOrderBook<Listener>::reduce(OrderId id, Qty new_qty) noexcept {
  auto hit = id_index_.find(id);
  if (hit == id_index_.end())
    return false;

  const Locator &loc = hit->second;
  Order &ord = *loc.it;

  if (new_qty == 0 || new_qty >= ord.qty)
    return false;

  const Qty delta = ord.qty - new_qty;
  ord.qty = new_qty;

  if (loc.side == Side::Buy)
    bids_.find(loc.price)->second.total_qty -= delta;
  else
    asks_.find(loc.price)->second.total_qty -= delta;
  return true;
}

// ─── execute ──────────────────────────────────────────────────────────

template <typename Listener>
inline bool MapOrderBook<Listener>::execute(OrderId id, Qty exec_qty) noexcept {
  auto hit = id_index_.find(id);
  if (hit == id_index_.end())
    return false;

  const Locator loc = hit->second;
  Order &ord = *loc.it;

  if (exec_qty == 0 || exec_qty > ord.qty)
    return false;

  ord.qty -= exec_qty;
  const bool gone = (ord.qty == 0);

  if (loc.side == Side::Buy) {
    auto lvl_it = bids_.find(loc.price);
    lvl_it->second.total_qty -= exec_qty;
    if (gone) {
      lvl_it->second.orders.erase(loc.it);
      if (lvl_it->second.orders.empty())
        bids_.erase(lvl_it);
    }
  } else {
    auto lvl_it = asks_.find(loc.price);
    lvl_it->second.total_qty -= exec_qty;
    if (gone) {
      lvl_it->second.orders.erase(loc.it);
      if (lvl_it->second.orders.empty())
        asks_.erase(lvl_it);
    }
  }

  if (gone)
    id_index_.erase(hit);
  return true;
}

// ─── qty_at ───────────────────────────────────────────────────────────

template <typename Listener>
inline Qty MapOrderBook<Listener>::qty_at(Side s, Price price) const {
  if (s == Side::Buy) {
    auto it = bids_.find(price);
    return it == bids_.end() ? Qty{0} : it->second.total_qty;
  } else {
    auto it = asks_.find(price);
    return it == asks_.end() ? Qty{0} : it->second.total_qty;
  }
}

// ─── apply (market-data interface) ────────────────────────────────────

template <typename Listener>
inline void MapOrderBook<Listener>::apply(const md::Add &a) {
  const Bbo before = snapshot_bbo();
  Order o{};
  o.id = a.id; o.side = a.side; o.price = a.px; o.qty = a.qty; o.ts = a.ts;
  rest_(o);
  notify_(before);
}

template <typename Listener>
inline void MapOrderBook<Listener>::apply(const md::Cancel &c) {
  const Bbo before = snapshot_bbo();
  if (cancel(c.id))
    notify_(before);
}

template <typename Listener>
inline void MapOrderBook<Listener>::apply(const md::Reduce &r) {
  const Bbo before = snapshot_bbo();
  if (reduce(r.id, r.new_qty))
    notify_(before);
}

template <typename Listener>
inline void MapOrderBook<Listener>::apply(const md::Exec &x) {
  const Bbo before = snapshot_bbo();
  if (execute(x.id, x.exec_qty)) {
    listener_->on_trade(sym_, x.px, x.exec_qty, x.trade_id);
    notify_(before);
  }
}

template <typename Listener>
inline void MapOrderBook<Listener>::apply(const md::Replace &r) {
  const Bbo before = snapshot_bbo();
  cancel(r.old_id);
  Order o{};
  o.id = r.new_id; o.side = r.side; o.price = r.px; o.qty = r.qty; o.ts = r.ts;
  rest_(o);
  notify_(before);
}

template <typename Listener>
inline void MapOrderBook<Listener>::apply(const md::Trade &t) {
  listener_->on_trade(sym_, t.px, t.qty, t.trade_id);
}

template <typename Listener>
inline void MapOrderBook<Listener>::apply(const md::Clear &) {
  const Bbo before = snapshot_bbo();
  bids_.clear();
  asks_.clear();
  id_index_.clear();
  notify_(before);
}

} // namespace hft::core
