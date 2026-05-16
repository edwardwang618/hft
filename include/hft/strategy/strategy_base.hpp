#pragma once

#include <hft/md/md_event.hpp>
#include <hft/strategy/i_order_gateway.hpp>

namespace hft::strategy {

// Non-CRTP base for strategies. Holds a gateway reference and provides
// default no-op listener hooks. Derived classes override what they need.
//
// Usage:
//   struct MyStrategy : StrategyBase {
//     using StrategyBase::StrategyBase;  // inherit constructor
//     void on_bbo(md::SymbolId sym, Price bid, Qty bq, Price ask, Qty aq)
//     noexcept override { ... }
//   };
//   ArrayOrderBook<MyStrategy> book(strategy, sym, min_px, ticks, tick);
class StrategyBase {
public:
  explicit StrategyBase(IOrderGateway &gw) : gateway_(gw) {}
  virtual ~StrategyBase() = default;

  // Called when the best bid or ask price/qty changes.
  // bid==0 or ask==0 means that side is empty.
  virtual void on_bbo(md::SymbolId, Price, Qty, Price, Qty) noexcept {}

  // Called when the book changes but BBO does not.
  // The Book template lets you call qty_at(), num_bid_levels(), etc.
  // Default is a no-op; override when you need level-2 data.
  template <class Book> void on_depth(md::SymbolId, const Book &) noexcept {}

  // Called for every passive execution visible in the feed.
  virtual void on_trade(md::SymbolId, Price, Qty, md::TradeId) noexcept {}

protected:
  IOrderGateway &gateway_;

  void send_new(md::SymbolId sym, Side side, Price px, Qty qty) {
    gateway_.send_new(sym, side, px, qty);
  }
  void send_cancel(OrderId id) { gateway_.send_cancel(id); }
  void send_replace(OrderId old_id, Price new_px, Qty new_qty) {
    gateway_.send_replace(old_id, new_px, new_qty);
  }
};

} // namespace hft::strategy
