// include/hft/strategy/simple_quoter.hpp
// Market-making strategy: quotes one lot on each side at the current BBO.
// 做市策略：在当前最优买卖价各挂一笔报价。
#pragma once

#include <hft/strategy/strategy_base.hpp>

namespace hft::strategy {

// Posts a passive bid and ask at the current BBO for a single symbol.
// Cancels and requotes whenever the BBO price changes.
// 在单一品种的当前 BBO 各挂一笔被动限价单。BBO 价格变动时撤单并重新报价。
//
// Lifecycle assumptions (no OrderManager yet):
//   - Fills are not tracked; a fill leaves bid_id_ / ask_id_ dangling until
//     the next BBO update triggers a requote.
//   - Replace is not used; cancel-then-new is simpler for this stage.
// 生命周期假设（尚无 OrderManager）：
//   - 成交不被追踪；成交后对应 ID 悬空，直到下次 BBO 变动触发重新报价。
//   - 不使用 Replace；此阶段用撤单再下单更简单。
class SimpleQuoter : public StrategyBase {
public:
  struct Params {
    md::SymbolId sym;
    Qty          qty; // shares per order / 每笔报价数量
  };

  SimpleQuoter(IOrderGateway &gw, Params p)
      : StrategyBase(gw), p_(p) {}

  void on_bbo(md::SymbolId sym,
              Price bid, Qty /*bq*/,
              Price ask, Qty /*aq*/) noexcept override {
    if (sym != p_.sym) return;

    // Cancel stale bid if price moved. / 买价变动时撤销旧买单。
    if (bid_id_ != 0 && bid != bid_px_) {
      send_cancel(bid_id_);
      bid_id_ = 0;
    }
    // Cancel stale ask if price moved. / 卖价变动时撤销旧卖单。
    if (ask_id_ != 0 && ask != ask_px_) {
      send_cancel(ask_id_);
      ask_id_ = 0;
    }

    // Post new bid if the side is non-empty and we have no live order.
    // 若买方非空且无在途买单，发送新买单。
    if (bid != 0 && bid_id_ == 0) {
      bid_id_ = send_new(p_.sym, Side::Buy, bid, p_.qty);
      bid_px_ = bid;
    }
    // Post new ask if the side is non-empty and we have no live order.
    // 若卖方非空且无在途卖单，发送新卖单。
    if (ask != 0 && ask_id_ == 0) {
      ask_id_ = send_new(p_.sym, Side::Sell, ask, p_.qty);
      ask_px_ = ask;
    }
  }

  // Diagnostics / 诊断接口
  OrderId bid_id() const noexcept { return bid_id_; }
  OrderId ask_id() const noexcept { return ask_id_; }
  Price   bid_px() const noexcept { return bid_px_; }
  Price   ask_px() const noexcept { return ask_px_; }

private:
  Params  p_;
  OrderId bid_id_{0}, ask_id_{0}; // 0 = no live order / 0 表示无在途订单
  Price   bid_px_{0}, ask_px_{0};
};

} // namespace hft::strategy
