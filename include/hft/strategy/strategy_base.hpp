// include/hft/strategy/strategy_base.hpp
// Non-CRTP polymorphic base for strategies.
// 策略的非 CRTP 多态基类。
#pragma once

#include <hft/md/md_event.hpp>
#include <hft/strategy/i_order_gateway.hpp>
#include <string_view>

namespace hft::strategy {

// Non-CRTP base for strategies. Holds a gateway reference and provides
// default no-op listener hooks. Derived classes override what they need.
// 策略的非 CRTP 基类。持有网关引用，并提供默认空操作监听器钩子。
// 派生类按需覆盖所需方法。
//
// Usage / 用法：
//   struct MyStrategy : StrategyBase {
//     using StrategyBase::StrategyBase;  // inherit constructor / 继承构造函数
//     void on_bbo(md::SymbolId sym, Price bid, Qty bq, Price ask, Qty aq)
//         noexcept override { ... }
//   };
//   ArrayOrderBook<MyStrategy> book(strategy, sym, min_px, ticks, tick);
//
class StrategyBase {
public:
  explicit StrategyBase(IOrderGateway &gw) : gateway_(gw) {}
  virtual ~StrategyBase() = default;

  // Called when the best bid or ask price/qty changes.
  // bid==0 or ask==0 means that side is empty.
  // BBO（最优买卖价）价格或数量变动时调用。bid==0 或 ask==0 表示该方向为空。
  virtual void on_bbo(md::SymbolId, Price, Qty, Price, Qty) noexcept {}

  // Called when the book changes but BBO does not.
  // The Book template lets you call qty_at(), num_bid_levels(), etc.
  // Default is a no-op; override when you need level-2 data.
  // 簿状态变化但 BBO 不变时调用。Book 模板参数支持调用
  // qty_at()、num_bid_levels() 等。 默认为空操作；需要二档数据时覆盖此方法。
  template <class Book> void on_depth(md::SymbolId, const Book &) noexcept {}

  // Called for every passive execution visible in the feed.
  // 每次行情中可见的被动成交时调用。
  virtual void on_trade(md::SymbolId, Price, Qty, md::TradeId) noexcept {}

  // ── Order lifecycle callbacks (called by OrderManager) ────────────
  // 订单生命周期回调（由 OrderManager 调用）

  // Exchange accepted our order. 交易所接受订单。
  virtual void on_ack(OrderId /*clord_id*/, OrderId /*exch_id*/) noexcept {}

  // Our order was filled (partially or fully). filled_qty = this fill only.
  // 订单成交（部分或全部）。filled_qty 为本次成交量。
  virtual void on_fill(OrderId /*clord_id*/, Qty /*filled_qty*/, Price /*px*/) noexcept {}

  // Our cancel was confirmed. 撤单确认。
  virtual void on_cancel_ack(OrderId /*clord_id*/) noexcept {}

  // Our order was rejected before reaching the book. 订单在入簿前被拒。
  virtual void on_reject(OrderId /*clord_id*/, std::string_view /*reason*/) noexcept {}

protected:
  IOrderGateway &gateway_;

  // Convenience wrappers forwarding to the gateway.
  // 转发到网关的便利包装函数。
  OrderId send_new(md::SymbolId sym, Side side, Price px, Qty qty) {
    return gateway_.send_new(sym, side, px, qty);
  }
  void send_cancel(OrderId id) { gateway_.send_cancel(id); }
  void send_replace(OrderId old_id, Price new_px, Qty new_qty) {
    gateway_.send_replace(old_id, new_px, new_qty);
  }
};

} // namespace hft::strategy
