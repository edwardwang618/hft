// include/hft/core/order_book.hpp
#pragma once

#include "hft/core/types.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <list>
#include <map>
#include <optional>
#include <unordered_map>
#include <utility>

namespace hft::core {

using Side = hft::Side;

// ─── A single price level: FIFO of orders at one price ────────────────
struct PriceLevel {
  Qty total_qty{0};
  std::list<Order> orders; // iterator-stable FIFO
};

// ─── Limit Order Book (single instrument) ─────────────────────────────
class OrderBook {
public:
  // Fired for every execution. (maker = resting order, taker = incoming).
  using TradeFn = std::function<void(OrderId maker, OrderId taker, Price, Qty)>;

  void set_trade_callback(TradeFn fn) { on_trade_ = std::move(fn); }

  // Add a limit order. Crosses against the book first; residual rests.
  void add_limit(Order o);

  // Cancel by id. No-op + returns false if id unknown.
  bool cancel(OrderId id);

  // 部分撤销: 把 id 的剩余量改成 new_qty.
  // 要求 0 < new_qty < 当前 qty. new_qty >= 当前 qty 视为 no-op 返回 false.
  // 不改变队列优先级 (时间戳不变).
  // 返回 true = 成功修改; false = id 不存在或参数无效.
  bool reduce(OrderId id, Qty new_qty) noexcept;

  // 被动成交: 把 id 的剩余量减去 exec_qty. 减到 0 则从 book 移除.
  // 返回 true = 成功; false = id 不存在 或 exec_qty > 当前 qty.
  bool execute(OrderId id, Qty exec_qty) noexcept;

  // Top-of-book. std::nullopt if that side is empty.
  std::optional<Price> best_bid() const {
    return bids_.empty() ? std::nullopt : std::optional{bids_.begin()->first};
  }
  std::optional<Price> best_ask() const {
    return asks_.empty() ? std::nullopt : std::optional{asks_.begin()->first};
  }

  // Aggregate resting qty at a given price/side. 0 if no level.
  Qty qty_at(Side s, Price price) const;

  std::size_t num_orders() const noexcept { return id_index_.size(); }
  std::size_t num_bid_levels() const noexcept { return bids_.size(); }
  std::size_t num_ask_levels() const noexcept { return asks_.size(); }

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

  BidMap bids_;
  AskMap asks_;
  std::unordered_map<OrderId, Locator> id_index_;
  TradeFn on_trade_;

  // Match a taker order against the opposite book. Updates taker.qty in place.
  // Template so we can pass either BidMap or AskMap; the cross-check differs
  // only in direction (buyer needs price ≥ ask, seller needs price ≤ bid).
  template <typename OppMap> void match_(OppMap &opp, Order &taker);

  // Insert residual into own book + index.
  void rest_(Order o);
};

// ─── template impl (must live in header) ──────────────────────────────
template <typename OppMap> void OrderBook::match_(OppMap &opp, Order &taker) {
  while (taker.qty > 0 && !opp.empty()) {
    auto lvl_it = opp.begin();
    const Price best = lvl_it->first;

    // Price-cross check. Buyer matches if willing_to_pay >= ask;
    // seller matches if willing_to_accept <= bid.
    const bool crosses = (taker.side == Side::Buy) ? (taker.price >= best)
                                                   : (taker.price <= best);
    if (!crosses)
      break;

    auto &lvl = lvl_it->second;

    // Sweep orders at this level in FIFO order.
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

inline void OrderBook::add_limit(Order o) {
  if (o.side == Side::Buy)
    match_(asks_, o);
  else
    match_(bids_, o);

  if (o.qty > 0)
    rest_(std::move(o));
}

inline void OrderBook::rest_(Order o) {
  if (o.side == Side::Buy) {
    auto &lvl = bids_[o.price];
    lvl.orders.push_back(o);
    lvl.total_qty += o.qty;
    auto it = std::prev(lvl.orders.end());
    id_index_.emplace(o.id, Locator{Side::Buy, o.price, it});
  } else {
    auto &lvl = asks_[o.price];
    lvl.orders.push_back(o);
    lvl.total_qty += o.qty;
    auto it = std::prev(lvl.orders.end());
    id_index_.emplace(o.id, Locator{Side::Sell, o.price, it});
  }
}

inline bool OrderBook::cancel(OrderId id) {
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

inline bool OrderBook::reduce(OrderId id, Qty new_qty) noexcept {
  auto hit = id_index_.find(id);
  if (hit == id_index_.end())
    return false;

  const Locator &loc = hit->second;
  Order &ord = *loc.it;

  if (new_qty == 0 || new_qty >= ord.qty)
    return false;

  const Qty delta = ord.qty - new_qty;
  ord.qty = new_qty;

  if (loc.side == Side::Buy) {
    bids_.find(loc.price)->second.total_qty -= delta;
  } else {
    asks_.find(loc.price)->second.total_qty -= delta;
  }
  return true;
}

inline bool OrderBook::execute(OrderId id, Qty exec_qty) noexcept {
  auto hit = id_index_.find(id);
  if (hit == id_index_.end())
    return false;

  const Locator loc = hit->second; // copy, 因为下面可能 erase(hit)
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

inline Qty OrderBook::qty_at(Side s, Price price) const {
  if (s == Side::Buy) {
    auto it = bids_.find(price);
    return it == bids_.end() ? Qty{0} : it->second.total_qty;
  } else {
    auto it = asks_.find(price);
    return it == asks_.end() ? Qty{0} : it->second.total_qty;
  }
}

} // namespace hft::core