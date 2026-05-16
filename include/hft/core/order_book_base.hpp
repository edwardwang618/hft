// include/hft/core/order_book_base.hpp
// CRTP base shared by ArrayOrderBook and MapOrderBook.
// ArrayOrderBook 和 MapOrderBook 共享的 CRTP 基类。
#pragma once

#include "hft/core/null_listener.hpp"
#include "hft/core/types.hpp"
#include "hft/md/md_event.hpp"

#include <concepts>
#include <functional>

namespace hft::core {

// ─── OrderBookBase ────────────────────────────────────────────────────
//
// CRTP base for limit order books. Holds the Listener pointer, symbol id,
// optional trade callback, and all market-data apply() dispatch logic.
// Concrete book state (bids, asks, id_index) lives in the derived class.
//
// 限价订单簿的 CRTP 基类。持有监听器指针、品种 ID、可选成交回调
// 以及所有行情 apply() 分发逻辑。具体簿状态（买/卖档、订单索引）
// 由派生类持有。
//
// Derived must provide (private, accessed via friend Base):
// 派生类须提供（private，通过 friend Base 访问）：
//
//   Bbo  snapshot_bbo() const noexcept   — current BBO / 当前最优价快照
//   bool price_valid_(Price) const noexcept   — bounds check or always-true
//                                              — 价格边界检查或恒返回 true
//   void rest_(Order)        — place without crossing / 无撮合挂单
//   void clear_state_()      — wipe all book state / 清空所有簿状态
//   // plus public: cancel(), reduce(), execute()
//
template <typename Derived, typename Listener = detail::NullListener>
class OrderBookBase {
public:
  // Matching-engine callback type: invoked on each internal fill.
  // 撮合引擎回调类型：每次内部成交时调用。
  using TradeFn = std::function<void(OrderId maker, OrderId taker, Price, Qty)>;

  // BBO snapshot: 0 on a side means that side is empty.
  // BBO 快照：某方向为 0 表示该方向为空。
  struct Bbo {
    Price bid{0};
    Qty bid_qty{0};
    Price ask{0};
    Qty ask_qty{0};
    bool operator==(const Bbo &) const noexcept = default;
  };

  // Optional matching-engine callback; call before applying feed events.
  // 可选撮合引擎回调；在处理行情事件前设置。
  void set_trade_callback(TradeFn fn) { on_trade_ = std::move(fn); }

  // ── Market-data apply interface ──────────────────────────────────
  // 行情 apply 接口：根据行情事件更新簿并触发监听器回调。
  //
  // Dispatch rules (mutually exclusive per event):
  // 分发规则（每次事件互斥触发）：
  //   BBO changed  → listener.on_bbo only   BBO 变动 → 仅触发 on_bbo
  //   BBO stable   → listener.on_depth only  BBO 不变 → 仅触发 on_depth
  //   Trade event  → listener.on_trade (plus whichever book callback fires)
  //                  成交事件 → on_trade（加上对应的簿回调）

  void apply(const md::Add &a) {
    if (!derived().price_valid_(a.px))
      return;
    const Bbo before = derived().snapshot_bbo();
    Order o{};
    o.id = a.id;
    o.side = a.side;
    o.price = a.px;
    o.qty = a.qty;
    o.ts = a.ts;
    derived().rest_(o);
    notify_(before);
  }

  void apply(const md::Cancel &c) {
    const Bbo before = derived().snapshot_bbo();
    if (derived().cancel(c.id))
      notify_(before);
  }

  void apply(const md::Reduce &r) {
    const Bbo before = derived().snapshot_bbo();
    if (derived().reduce(r.id, r.new_qty))
      notify_(before);
  }

  void apply(const md::Exec &x) {
    const Bbo before = derived().snapshot_bbo();
    if (derived().execute(x.id, x.exec_qty)) {
      listener_->on_trade(sym_, x.px, x.exec_qty, x.trade_id);
      notify_(before);
    }
  }

  void apply(const md::Replace &r) {
    if (!derived().price_valid_(r.px))
      return;
    const Bbo before = derived().snapshot_bbo();
    derived().cancel(r.old_id);
    Order o{};
    o.id = r.new_id;
    o.side = r.side;
    o.price = r.px;
    o.qty = r.qty;
    o.ts = r.ts;
    derived().rest_(o);
    notify_(before);
  }

  // Non-displayed trade: fire on_trade only, no book state change.
  // 非显示成交（隐藏单/集合竞价）：仅触发 on_trade，不修改簿状态。
  void apply(const md::Trade &t) {
    listener_->on_trade(sym_, t.px, t.qty, t.trade_id);
  }

  void apply(const md::Clear &) {
    const Bbo before = derived().snapshot_bbo();
    derived().clear_state_();
    notify_(before);
  }

protected:
  // Strategy/production constructor: listener fires on every book event.
  // 策略/生产环境构造函数：每次行情事件均触发监听器。
  OrderBookBase(Listener &l, md::SymbolId sym) : listener_(&l), sym_(sym) {}

  // Standalone/test constructor: only when Listener is NullListener.
  // 独立/测试构造函数：仅在 Listener 为 NullListener 时可用。
  OrderBookBase()
    requires std::same_as<Listener, detail::NullListener>
      : listener_(&detail::g_null_listener), sym_(0) {}

  // Protected so derived match methods (match_buy_, match_sell_, match_) can
  // invoke it. protected 使派生类的撮合方法可直接调用。
  TradeFn on_trade_;

private:
  Listener *listener_;
  md::SymbolId sym_;

  Derived &derived() noexcept { return *static_cast<Derived *>(this); }
  const Derived &derived() const noexcept {
    return *static_cast<const Derived *>(this);
  }

  // Fire on_bbo if the BBO changed, otherwise on_depth.
  // BBO 变动则触发 on_bbo，否则触发 on_depth。
  void notify_(const Bbo &before) noexcept {
    const Bbo after = derived().snapshot_bbo();
    if (after != before)
      listener_->on_bbo(sym_, after.bid, after.bid_qty, after.ask,
                        after.ask_qty);
    else
      listener_->on_depth(sym_, derived());
  }
};

} // namespace hft::core
