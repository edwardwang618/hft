// include/hft/strategy/order_manager.hpp
// Tracks live orders and routes exchange reports back to the strategy.
// 追踪在途订单，将交易所回报路由回策略。
#pragma once

#include <hft/strategy/i_order_gateway.hpp>
#include <hft/strategy/i_order_listener.hpp>
#include <hft/strategy/strategy_base.hpp>

#include <string_view>
#include <unordered_map>

namespace hft::strategy {

// ── Order state machine ───────────────────────────────────────────────
// 订单状态机

enum class OrderState : uint8_t {
  Pending,        // sent to wire, no ack yet      已发线路，等待确认
  Live,           // acked, resting on book         已确认，在簿
  PendingCancel,  // cancel sent, no confirm yet    已发撤单，等待确认
  Filled,         // fully filled (terminal)        全部成交（终态）
  Cancelled,      // cancel confirmed (terminal)    撤单确认（终态）
  Rejected,       // rejected by exchange (terminal) 被拒（终态）
};

// ── Per-order record ──────────────────────────────────────────────────
// Named ManagedOrder to avoid collision with hft::Order (order-book record).
// 命名为 ManagedOrder 以避免与 hft::Order（order-book 记录）冲突。

struct ManagedOrder {
  OrderId      clord_id{0};
  OrderId      exch_id{0};    // 0 until ack / ack 前为 0
  md::SymbolId sym{0};
  Side         side{Side::Buy};
  Price        px{0};
  Qty          qty{0};
  Qty          filled_qty{0}; // cumulative fills / 累计成交量
  OrderState   state{OrderState::Pending};
};

// ─────────────────────────────────────────────────────────────────────
//
// OrderManager
//
// Sits between Strategy and the exchange wire:
//   - Implements IOrderGateway  (Strategy calls send_new / send_cancel)
//   - Implements IOrderListener (Exchange calls on_ack / on_fill / ...)
//
// IExchangeGateway template parameter: the concrete wire layer.
// Must provide: send_new(clord_id, sym, side, px, qty)
//               send_cancel(clord_id, exch_id)
//
template <class IExchangeGateway>
class OrderManager : public IOrderGateway, public IOrderListener {
public:
  explicit OrderManager(StrategyBase &strategy, IExchangeGateway &wire)
      : strategy_(strategy), wire_(wire) {}

  // ── IOrderGateway (called by Strategy) ───────────────────────────

  // Assign a clord_id, record the order, forward to wire.
  // 分配 clord_id，记录订单，转发给线路。
  OrderId send_new(md::SymbolId sym, Side side, Price px, Qty qty) override {
    const OrderId id = next_clord_id_++;
    orders_.emplace(id, ManagedOrder{id, 0, sym, side, px, qty, 0, OrderState::Pending});
    wire_.send_new(id, sym, side, px, qty);
    return id;
  }

  // Transition to PendingCancel and forward cancel to wire.
  // 转入 PendingCancel 状态，将撤单请求转发给线路。
  void send_cancel(OrderId clord_id) override {
    auto it = orders_.find(clord_id);
    if (it == orders_.end()) return;
    ManagedOrder &o = it->second;
    if (o.state != OrderState::Live) return; // only cancel live orders
    o.state = OrderState::PendingCancel;
    wire_.send_cancel(clord_id, o.exch_id);
  }

  void send_replace(OrderId /*old_id*/, Price /*px*/, Qty /*qty*/) override {
    // Not implemented yet; cancel-then-new is used for now.
    // 暂未实现；当前用撤单再下单代替。
  }

  // ── IOrderListener (called by exchange gateway) ───────────────────

  void on_ack(OrderId clord_id, OrderId exch_id) override {
    auto it = orders_.find(clord_id);
    if (it == orders_.end()) return;
    ManagedOrder &o = it->second;
    o.exch_id = exch_id;
    o.state   = OrderState::Live;
    exch_to_clord_.emplace(exch_id, clord_id);
    strategy_.on_ack(clord_id, exch_id);
  }

  void on_fill(OrderId exch_id, Qty filled_qty, Price px) override {
    auto eit = exch_to_clord_.find(exch_id);
    if (eit == exch_to_clord_.end()) return;
    ManagedOrder &o = orders_.at(eit->second);
    o.filled_qty   += filled_qty;
    update_position_(o.sym, o.side, filled_qty);
    if (o.filled_qty >= o.qty) {
      o.state = OrderState::Filled;
      exch_to_clord_.erase(eit);
    }
    strategy_.on_fill(o.clord_id, filled_qty, px);
  }

  void on_cancel_ack(OrderId exch_id) override {
    auto eit = exch_to_clord_.find(exch_id);
    if (eit == exch_to_clord_.end()) return;
    ManagedOrder &o = orders_.at(eit->second);
    o.state = OrderState::Cancelled;
    exch_to_clord_.erase(eit);
    strategy_.on_cancel_ack(o.clord_id);
  }

  void on_reject(OrderId clord_id, std::string_view reason) override {
    auto it = orders_.find(clord_id);
    if (it == orders_.end()) return;
    it->second.state = OrderState::Rejected;
    strategy_.on_reject(clord_id, reason);
  }

  // ── Diagnostics ───────────────────────────────────────────────────

  const ManagedOrder *find(OrderId clord_id) const noexcept {
    auto it = orders_.find(clord_id);
    return it == orders_.end() ? nullptr : &it->second;
  }

  // Net position for a symbol: positive = long, negative = short.
  // 品种净持仓：正数为多头，负数为空头。
  int64_t position(md::SymbolId sym) const noexcept {
    auto it = position_.find(sym);
    return it == position_.end() ? 0 : it->second;
  }

  std::size_t order_count() const noexcept { return orders_.size(); }

private:
  StrategyBase     &strategy_;
  IExchangeGateway &wire_;
  OrderId           next_clord_id_{1};

  std::unordered_map<OrderId, ManagedOrder> orders_;        // clord_id → ManagedOrder
  std::unordered_map<OrderId, OrderId>      exch_to_clord_; // exch_id  → clord_id
  std::unordered_map<md::SymbolId, int64_t> position_;      // sym → net qty

  void update_position_(md::SymbolId sym, Side side, Qty qty) {
    int64_t delta = (side == Side::Buy) ? static_cast<int64_t>(qty)
                                        : -static_cast<int64_t>(qty);
    position_[sym] += delta;
  }
};

} // namespace hft::strategy
