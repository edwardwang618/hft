// include/hft/core/array_order_book.hpp
// Price-indexed flat-array limit order book.
// 以价格索引数组为底层结构的限价订单簿。
#pragma once

#include "hft/core/null_listener.hpp"
#include "hft/core/order_book_base.hpp"
#include "hft/core/types.hpp"
#include "hft/md/md_event.hpp"

#include <algorithm>
#include <cassert>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

namespace hft::core {

// ─── ArrayOrderBook ───────────────────────────────────────────────────
//
// Order book backed by a flat price-indexed array.
// 以价格索引数组为底层结构的限价订单簿。
//
// Compared to MapOrderBook:
// 与 MapOrderBook 对比：
//   Level lookup : O(1) direct array index      vs O(log N) tree traversal
//   档位查找    : O(1) 直接数组索引            vs O(log N) 树遍历
//   Best bid/ask: O(1) maintained integer        vs O(log N) map::begin
//   最优价维护  : O(1) 整数维护                vs O(log N) map 首元素
//   Per-level FIFO: contiguous vector + head ptr vs pointer-chained list
//   档内 FIFO   : 连续向量 + head 指针         vs 指针链表
//   Compaction  : amortised O(1) per order       vs list iterator erasure
//   压缩        : 每笔订单摊销 O(1)             vs 链表迭代器删除
//
// Trade-off: requires knowing the price range at construction.
// 代价：构造时需已知价格范围。
//
// Listener contract (template — zero virtual, fully inlinable):
// 监听器接口（模板，无虚函数，全链路可内联）：
//   void on_bbo(SymbolId, Price bid, Qty bq, Price ask, Qty aq) noexcept
//   template<class Book> void on_depth(SymbolId, const Book&) noexcept
//   void on_trade(SymbolId, Price, Qty, TradeId) noexcept
//
template <typename Listener = detail::NullListener>
class ArrayOrderBook : public OrderBookBase<ArrayOrderBook<Listener>, Listener> {
  using Base = OrderBookBase<ArrayOrderBook<Listener>, Listener>;
  friend Base; // Allow base to call private rest_/price_valid_/clear_state_.
               // 允许基类调用私有方法 rest_/price_valid_/clear_state_。

public:
  using Bbo     = typename Base::Bbo;     // Dependent base name, pull into scope.
  using TradeFn = typename Base::TradeFn; // 依赖基类名，引入当前作用域。

  // ── Constructors ──────────────────────────────────────────────────
  // 构造函数

  // Strategy/production constructor: listener fires on every book event.
  // 策略/生产环境构造函数：每次行情事件均触发监听器。
  ArrayOrderBook(Listener &l, md::SymbolId sym,
                 Price min_price, uint32_t num_ticks, Price tick = 1)
      : Base(l, sym),
        min_price_(min_price), tick_(tick), num_ticks_(num_ticks),
        bids_(num_ticks), asks_(num_ticks) {}

  // Standalone/test constructor: no listener required.
  // 独立/测试构造函数：无需监听器，仅在 NullListener 时可用。
  ArrayOrderBook(Price min_price, uint32_t num_ticks, Price tick = 1)
      requires std::same_as<Listener, detail::NullListener>
      : Base(),
        min_price_(min_price), tick_(tick), num_ticks_(num_ticks),
        bids_(num_ticks), asks_(num_ticks) {}

  // ── Core order operations (matching engine path) ──────────────────
  // 核心订单操作（撮合引擎路径）

  // Returns false if o.price is outside [min_price_, min_price_ + num_ticks_*tick_).
  // 若 o.price 超出价格范围则返回 false。
  bool add_limit(Order o);
  bool cancel(OrderId id);
  bool reduce(OrderId id, Qty new_qty) noexcept;
  bool execute(OrderId id, Qty exec_qty) noexcept;

  // ── Queries ──────────────────────────────────────────────────────
  // 查询接口

  std::optional<Price> best_bid() const noexcept {
    if (best_bid_slot_ < 0) return std::nullopt;
    return slot_to_price(static_cast<uint32_t>(best_bid_slot_));
  }
  std::optional<Price> best_ask() const noexcept {
    if (best_ask_slot_ < 0) return std::nullopt;
    return slot_to_price(static_cast<uint32_t>(best_ask_slot_));
  }

  Qty         qty_at(Side s, Price price) const noexcept;
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
  // 每个价格点对应一个槽位（买卖两侧各自独立）。
  // Cancelled orders are tombstoned (qty=0); head advances past consumed entries.
  // Compaction runs when waste exceeds a threshold (amortised O(1)).
  // 已撤订单以墓碑标记（qty=0）；head 向前推进跳过已消耗条目。
  // 废弃条目超过阈值时执行压缩（摊销 O(1)）。
  struct Slot {
    Qty              total_qty{0};
    uint32_t         head{0};
    std::vector<Order> queue;
    bool empty() const noexcept { return total_qty == 0; }
  };

  // Points directly to an order in O(1) without any tree traversal.
  // O(1) 直接定位订单，无需树遍历。
  struct Locator {
    Side     side;
    uint32_t slot;
    uint32_t queue_idx;
  };

  Price    min_price_;
  Price    tick_;
  uint32_t num_ticks_;

  std::vector<Slot> bids_; // bids_[slot]: higher slot = higher price / 槽位越高价格越高
  std::vector<Slot> asks_; // asks_[slot]: lower  slot = lower  price / 槽位越低价格越低

  int32_t best_bid_slot_{-1}; // -1 = empty / -1 表示该方向为空
  int32_t best_ask_slot_{-1};

  std::size_t num_bid_levels_{0};
  std::size_t num_ask_levels_{0};

  std::unordered_map<OrderId, Locator> id_index_;

  // ── Price ↔ slot conversion ───────────────────────────────────────
  // 价格 ↔ 槽位转换

  uint32_t price_to_slot(Price p) const noexcept {
    return static_cast<uint32_t>((p - min_price_) / tick_);
  }
  Price slot_to_price(uint32_t s) const noexcept {
    return min_price_ + static_cast<Price>(s) * tick_;
  }

  // ── CRTP hooks called by OrderBookBase ────────────────────────────
  // OrderBookBase 通过 CRTP 调用的钩子（派生类私有实现）

  // Returns false when price is outside the pre-allocated slot range.
  // 价格超出预分配槽位范围时返回 false。
  bool price_valid_(Price p) const noexcept { return price_to_slot(p) < num_ticks_; }

  // Reset all book state (called by apply(Clear)).
  // 清空所有簿状态（由 apply(Clear) 调用）。
  void clear_state_() {
    bids_.assign(num_ticks_, Slot{});
    asks_.assign(num_ticks_, Slot{});
    best_bid_slot_ = -1;
    best_ask_slot_ = -1;
    num_bid_levels_ = 0;
    num_ask_levels_ = 0;
    id_index_.clear();
  }

  void rest_(Order o);

  // ── Compaction ────────────────────────────────────────────────────
  // 队列压缩

  // Remove the consumed prefix of sl.queue when waste exceeds threshold.
  // Updates queue_idx in id_index_ for all surviving orders.
  // 废弃条目超过阈值时移除 sl.queue 的已消耗前缀，并更新存活订单的索引。
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
  // 从 start 向下扫描，找到下一个非空买方槽位。
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
  // 从 start 向上扫描，找到下一个非空卖方槽位。
  void rescan_best_ask(int32_t start) {
    for (int32_t s = start; s < static_cast<int32_t>(num_ticks_); ++s) {
      if (!asks_[static_cast<uint32_t>(s)].empty()) {
        best_ask_slot_ = s;
        return;
      }
    }
    best_ask_slot_ = -1;
  }

  // ── Internal matching (add_limit path) ───────────────────────────
  // 内部撮合（add_limit 路径）

  void match_buy_(Order &taker) {
    while (taker.qty > 0 && best_ask_slot_ >= 0) {
      const uint32_t sidx = static_cast<uint32_t>(best_ask_slot_);
      if (taker.price < slot_to_price(sidx))
        break;
      Slot &sl = asks_[sidx];
      while (taker.qty > 0 && sl.head < static_cast<uint32_t>(sl.queue.size())) {
        Order &maker = sl.queue[sl.head];
        if (maker.qty == 0) { ++sl.head; continue; }
        const Qty traded = std::min(taker.qty, maker.qty);
        if (this->on_trade_)
          this->on_trade_(maker.id, taker.id, slot_to_price(sidx), traded);
        taker.qty -= traded;
        maker.qty -= traded;
        sl.total_qty -= traded;
        if (maker.qty == 0) { id_index_.erase(maker.id); ++sl.head; }
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
      while (taker.qty > 0 && sl.head < static_cast<uint32_t>(sl.queue.size())) {
        Order &maker = sl.queue[sl.head];
        if (maker.qty == 0) { ++sl.head; continue; }
        const Qty traded = std::min(taker.qty, maker.qty);
        if (this->on_trade_)
          this->on_trade_(maker.id, taker.id, slot_to_price(sidx), traded);
        taker.qty -= traded;
        maker.qty -= traded;
        sl.total_qty -= traded;
        if (maker.qty == 0) { id_index_.erase(maker.id); ++sl.head; }
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
// 无撮合挂单（MD 路径：直接静置，不与对手方撮合）

template <typename Listener>
inline void ArrayOrderBook<Listener>::rest_(Order o) {
  const uint32_t sidx = price_to_slot(o.price);

  if (o.side == Side::Buy) {
    Slot &sl = bids_[sidx];
    const bool     was_empty = sl.empty();
    const uint32_t qidx      = static_cast<uint32_t>(sl.queue.size());
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
    const bool     was_empty = sl.empty();
    const uint32_t qidx      = static_cast<uint32_t>(sl.queue.size());
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
// 挂限价单（撮合引擎路径：先撮合，剩余量静置）

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
// 整单撤销

template <typename Listener>
inline bool ArrayOrderBook<Listener>::cancel(OrderId id) {
  auto it = id_index_.find(id);
  if (it == id_index_.end())
    return false;

  const Locator loc = it->second;
  id_index_.erase(it);

  Slot  &sl  = (loc.side == Side::Buy) ? bids_[loc.slot] : asks_[loc.slot];
  Order &ord = sl.queue[loc.queue_idx];
  sl.total_qty -= ord.qty;
  ord.qty = 0; // tombstone / 墓碑标记

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
// 部分撤销：将剩余量缩减到 new_qty

template <typename Listener>
inline bool ArrayOrderBook<Listener>::reduce(OrderId id, Qty new_qty) noexcept {
  auto it = id_index_.find(id);
  if (it == id_index_.end())
    return false;

  const Locator &loc = it->second;
  Slot  &sl  = (loc.side == Side::Buy) ? bids_[loc.slot] : asks_[loc.slot];
  Order &ord = sl.queue[loc.queue_idx];

  if (new_qty == 0 || new_qty >= ord.qty)
    return false;

  sl.total_qty -= (ord.qty - new_qty);
  ord.qty = new_qty;
  return true;
}

// ─── execute ──────────────────────────────────────────────────────────
// 被动成交：从挂单扣除 exec_qty

template <typename Listener>
inline bool ArrayOrderBook<Listener>::execute(OrderId id, Qty exec_qty) noexcept {
  auto it = id_index_.find(id);
  if (it == id_index_.end())
    return false;

  const Locator loc = it->second;
  Slot  &sl  = (loc.side == Side::Buy) ? bids_[loc.slot] : asks_[loc.slot];
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
// 查询指定价格的总挂单量

template <typename Listener>
inline Qty ArrayOrderBook<Listener>::qty_at(Side s, Price price) const noexcept {
  const uint32_t sidx = price_to_slot(price);
  if (sidx >= num_ticks_)
    return 0;
  const Slot &sl = (s == Side::Buy) ? bids_[sidx] : asks_[sidx];
  return sl.total_qty;
}

} // namespace hft::core
