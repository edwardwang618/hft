// include/hft/core/map_order_book.hpp
// std::map-based limit order book; unbounded price range.
// 基于 std::map 的限价订单簿；支持任意价格范围。
#pragma once

#include "hft/core/null_listener.hpp"
#include "hft/core/order_book_base.hpp"
#include "hft/core/types.hpp"
#include "hft/md/md_event.hpp"

#include <concepts>
#include <cstddef>
#include <list>
#include <map>
#include <optional>
#include <unordered_map>
#include <utility>

namespace hft::core {

// ─── A single price level: iterator-stable FIFO of orders ─────────────
// 单个价格档位：迭代器稳定的 FIFO 订单队列
struct PriceLevel {
  Qty              total_qty{0};
  std::list<Order> orders; // iterator-stable FIFO / 迭代器稳定的 FIFO
};

// ─── MapOrderBook ─────────────────────────────────────────────────────
//
// Limit order book backed by std::map (red-black tree per side).
// Supports arbitrary price ranges with no pre-allocation.
// 基于 std::map（每方向一棵红黑树）的限价订单簿。
// 支持任意价格范围，无需预分配。
//
// Compared to ArrayOrderBook:
// 与 ArrayOrderBook 对比：
//   Level lookup : O(log N)  vs O(1) array index     / O(log N) vs O(1) 数组索引
//   Best bid/ask : O(log N)  vs O(1) maintained int  / O(log N) vs O(1) 整数维护
//   Price range  : unbounded vs fixed window          / 无界      vs 固定窗口
//
// Listener contract (template — zero virtual, fully inlinable):
// 监听器接口（模板，无虚函数，全链路可内联）：
//   void on_bbo(SymbolId, Price bid, Qty bq, Price ask, Qty aq) noexcept
//   template<class Book> void on_depth(SymbolId, const Book&) noexcept
//   void on_trade(SymbolId, Price, Qty, TradeId) noexcept
//
template <typename Listener = detail::NullListener>
class MapOrderBook : public OrderBookBase<MapOrderBook<Listener>, Listener> {
  using Base = OrderBookBase<MapOrderBook<Listener>, Listener>;
  friend Base; // Allow base to call private rest_/price_valid_/clear_state_.
               // 允许基类调用私有方法 rest_/price_valid_/clear_state_。

public:
  using Bbo     = typename Base::Bbo;     // Dependent base name, pull into scope.
  using TradeFn = typename Base::TradeFn; // 依赖基类名，引入当前作用域。

  // ── Constructors ─────────────────────────────────────────────────
  // 构造函数

  // Strategy/production constructor.
  // 策略/生产环境构造函数。
  MapOrderBook(Listener &l, md::SymbolId sym) : Base(l, sym) {}

  // Standalone/test constructor: only when Listener is NullListener.
  // 独立/测试构造函数：仅在 Listener 为 NullListener 时可用。
  MapOrderBook()
      requires std::same_as<Listener, detail::NullListener>
      : Base() {}

  // ── Core order operations (matching engine path) ──────────────────
  // 核心订单操作（撮合引擎路径）

  // Add a limit order: crosses against the book first; residual rests.
  // 挂限价单：先与对手方撮合，剩余量静置。
  void add_limit(Order o);

  // Cancel by id. Returns false if id unknown.
  // 按 ID 整单撤销。若 ID 未知则返回 false。
  bool cancel(OrderId id);

  // Partial cancel: reduce resting qty to new_qty.
  // Requires 0 < new_qty < current qty. Returns false on invalid args.
  // 部分撤销：将剩余量缩减到 new_qty。
  // 要求 0 < new_qty < 当前量；参数非法时返回 false。
  bool reduce(OrderId id, Qty new_qty) noexcept;

  // Passive execution: deduct exec_qty from resting order.
  // Returns false if id unknown or exec_qty > current qty.
  // 被动成交：从挂单扣除 exec_qty。
  // 若 ID 未知或 exec_qty > 当前量则返回 false。
  bool execute(OrderId id, Qty exec_qty) noexcept;

  // ── Queries ──────────────────────────────────────────────────────
  // 查询接口

  std::optional<Price> best_bid() const {
    return bids_.empty() ? std::nullopt : std::optional{bids_.begin()->first};
  }
  std::optional<Price> best_ask() const {
    return asks_.empty() ? std::nullopt : std::optional{asks_.begin()->first};
  }

  Qty         qty_at(Side s, Price price) const;
  std::size_t num_orders() const noexcept { return id_index_.size(); }
  std::size_t num_bid_levels() const noexcept { return bids_.size(); }
  std::size_t num_ask_levels() const noexcept { return asks_.size(); }

  Bbo snapshot_bbo() const noexcept {
    Bbo b;
    if (!bids_.empty()) {
      auto it   = bids_.begin();
      b.bid     = it->first;
      b.bid_qty = it->second.total_qty;
    }
    if (!asks_.empty()) {
      auto it   = asks_.begin();
      b.ask     = it->first;
      b.ask_qty = it->second.total_qty;
    }
    return b;
  }

private:
  // std::greater → bids_.begin() is the *highest* price (best bid)
  // std::less    → asks_.begin() is the *lowest*  price (best ask)
  // std::greater → bids_.begin() 为最高价（最优买价）
  // std::less    → asks_.begin() 为最低价（最优卖价）
  using BidMap = std::map<Price, PriceLevel, std::greater<Price>>;
  using AskMap = std::map<Price, PriceLevel, std::less<Price>>;

  struct Locator {
    Side                     side;
    Price                    price;
    std::list<Order>::iterator it; // stable as long as list isn't erased / 只要列表不被删除则稳定
  };

  BidMap bids_;
  AskMap asks_;
  std::unordered_map<OrderId, Locator> id_index_;

  // ── CRTP hooks called by OrderBookBase ────────────────────────────
  // OrderBookBase 通过 CRTP 调用的钩子（派生类私有实现）

  // MapOrderBook has no price bounds; always valid.
  // MapOrderBook 没有价格边界限制，始终有效。
  bool price_valid_(Price) const noexcept { return true; }

  // Reset all book state (called by apply(Clear)).
  // 清空所有簿状态（由 apply(Clear) 调用）。
  void clear_state_() {
    bids_.clear();
    asks_.clear();
    id_index_.clear();
  }

  void rest_(Order o);

  // ── Internal matching (add_limit path) ───────────────────────────
  // 内部撮合（add_limit 路径）

  template <typename OppMap>
  void match_(OppMap &opp, Order &taker);
};

// ─── match_ ───────────────────────────────────────────────────────────
// 内部撮合核心：对 opp 方向逐档撮合，直到 taker 剩余量为 0 或价格不满足

template <typename Listener>
template <typename OppMap>
void MapOrderBook<Listener>::match_(OppMap &opp, Order &taker) {
  while (taker.qty > 0 && !opp.empty()) {
    auto        lvl_it = opp.begin();
    const Price best   = lvl_it->first;
    const bool  crosses =
        (taker.side == Side::Buy) ? (taker.price >= best) : (taker.price <= best);
    if (!crosses)
      break;
    auto &lvl = lvl_it->second;
    while (taker.qty > 0 && !lvl.orders.empty()) {
      Order    &maker  = lvl.orders.front();
      const Qty traded = (taker.qty < maker.qty) ? taker.qty : maker.qty;
      if (this->on_trade_)
        this->on_trade_(maker.id, taker.id, best, traded);
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
// 无撮合挂单（MD 路径：直接静置，不与对手方撮合）

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
// 挂限价单（撮合引擎路径）

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
// 整单撤销

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
// 部分撤销：将剩余量缩减到 new_qty

template <typename Listener>
inline bool MapOrderBook<Listener>::reduce(OrderId id, Qty new_qty) noexcept {
  auto hit = id_index_.find(id);
  if (hit == id_index_.end())
    return false;
  const Locator &loc   = hit->second;
  Order         &ord   = *loc.it;
  if (new_qty == 0 || new_qty >= ord.qty)
    return false;
  const Qty delta = ord.qty - new_qty;
  ord.qty         = new_qty;
  if (loc.side == Side::Buy)
    bids_.find(loc.price)->second.total_qty -= delta;
  else
    asks_.find(loc.price)->second.total_qty -= delta;
  return true;
}

// ─── execute ──────────────────────────────────────────────────────────
// 被动成交：从挂单扣除 exec_qty

template <typename Listener>
inline bool MapOrderBook<Listener>::execute(OrderId id, Qty exec_qty) noexcept {
  auto hit = id_index_.find(id);
  if (hit == id_index_.end())
    return false;
  const Locator loc = hit->second;
  Order        &ord = *loc.it;
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
// 查询指定价格的总挂单量

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

} // namespace hft::core
