#include <hft/strategy/order_manager.hpp>
#include <gtest/gtest.h>
#include <string>
#include <vector>

using namespace hft;
using namespace hft::strategy;

namespace {

// ── Mock wire (exchange-side gateway) ─────────────────────────────────
// Records what OrderManager sent to the exchange.
// 记录 OrderManager 发往交易所的指令。

struct MockWire {
  struct NewReq    { OrderId clord_id; md::SymbolId sym; Side side; Price px; Qty qty; };
  struct CancelReq { OrderId clord_id; OrderId exch_id; };

  std::vector<NewReq>    sent_new;
  std::vector<CancelReq> sent_cancel;

  void send_new(OrderId cid, md::SymbolId sym, Side side, Price px, Qty qty) {
    sent_new.push_back({cid, sym, side, px, qty});
  }
  void send_cancel(OrderId cid, OrderId eid) {
    sent_cancel.push_back({cid, eid});
  }
};

// ── Recording strategy ────────────────────────────────────────────────
// Captures all order lifecycle callbacks from OrderManager.
// 捕获 OrderManager 的所有订单生命周期回调。

struct MockStrategy : StrategyBase {
  struct AckEv    { OrderId clord_id, exch_id; };
  struct FillEv   { OrderId clord_id; Qty qty; Price px; };
  struct CancelEv { OrderId clord_id; };
  struct RejectEv { OrderId clord_id; std::string reason; };

  std::vector<AckEv>    acks;
  std::vector<FillEv>   fills;
  std::vector<CancelEv> cancels;
  std::vector<RejectEv> rejects;

  // Provide a dummy gateway; MockStrategy won't call it in these tests.
  // 提供占位网关；MockStrategy 在这些测试中不会调用它。
  struct NullGateway : IOrderGateway {
    OrderId send_new(md::SymbolId, Side, Price, Qty) override { return 0; }
    void send_cancel(OrderId) override {}
    void send_replace(OrderId, Price, Qty) override {}
  } null_gw;

  MockStrategy() : StrategyBase(null_gw) {}

  void on_ack(OrderId cid, OrderId eid)              noexcept override { acks.push_back({cid, eid}); }
  void on_fill(OrderId cid, Qty qty, Price px)       noexcept override { fills.push_back({cid, qty, px}); }
  void on_cancel_ack(OrderId cid)                    noexcept override { cancels.push_back({cid}); }
  void on_reject(OrderId cid, std::string_view why)  noexcept override { rejects.push_back({cid, std::string(why)}); }
};

// ── Fixture ───────────────────────────────────────────────────────────

constexpr md::SymbolId SYM = 1;

struct Fx {
  MockStrategy strategy;
  MockWire     wire;
  OrderManager<MockWire> om{strategy, wire};
};

} // namespace

// ═════════════════════════════════════════════════════════════════════

TEST(OrderManager, SendNewRecordsOrderAndForwardsToWire) {
  Fx fx;
  OrderId cid = fx.om.send_new(SYM, Side::Buy, 100, 10);

  ASSERT_EQ(fx.wire.sent_new.size(), 1u);
  EXPECT_EQ(fx.wire.sent_new[0].clord_id, cid);
  EXPECT_EQ(fx.wire.sent_new[0].sym,      SYM);
  EXPECT_EQ(fx.wire.sent_new[0].side,     Side::Buy);
  EXPECT_EQ(fx.wire.sent_new[0].px,       100);
  EXPECT_EQ(fx.wire.sent_new[0].qty,      10u);

  const auto *o = fx.om.find(cid);
  ASSERT_NE(o, nullptr);
  EXPECT_EQ(o->state, OrderState::Pending);
  EXPECT_EQ(o->exch_id, 0u); // not acked yet
}

TEST(OrderManager, AckTransitionsToLiveAndCallsStrategy) {
  Fx fx;
  OrderId cid = fx.om.send_new(SYM, Side::Buy, 100, 10);

  fx.om.on_ack(cid, /*exch_id=*/9001);

  const auto *o = fx.om.find(cid);
  ASSERT_NE(o, nullptr);
  EXPECT_EQ(o->state,   OrderState::Live);
  EXPECT_EQ(o->exch_id, 9001u);

  ASSERT_EQ(fx.strategy.acks.size(), 1u);
  EXPECT_EQ(fx.strategy.acks[0].clord_id, cid);
  EXPECT_EQ(fx.strategy.acks[0].exch_id,  9001u);
}

TEST(OrderManager, FullFillUpdatesStateAndPosition) {
  Fx fx;
  OrderId cid = fx.om.send_new(SYM, Side::Buy, 100, 10);
  fx.om.on_ack(cid, 9001);

  fx.om.on_fill(9001, /*filled_qty=*/10, /*px=*/100);

  const auto *o = fx.om.find(cid);
  ASSERT_NE(o, nullptr);
  EXPECT_EQ(o->state,       OrderState::Filled);
  EXPECT_EQ(o->filled_qty,  10u);

  EXPECT_EQ(fx.om.position(SYM), 10);

  ASSERT_EQ(fx.strategy.fills.size(), 1u);
  EXPECT_EQ(fx.strategy.fills[0].clord_id, cid);
  EXPECT_EQ(fx.strategy.fills[0].qty,      10u);
  EXPECT_EQ(fx.strategy.fills[0].px,       100);
}

TEST(OrderManager, PartialFillsAccumulate) {
  Fx fx;
  OrderId cid = fx.om.send_new(SYM, Side::Buy, 100, 10);
  fx.om.on_ack(cid, 9001);

  fx.om.on_fill(9001, 4, 100); // partial
  fx.om.on_fill(9001, 6, 100); // rest

  EXPECT_EQ(fx.om.find(cid)->state,      OrderState::Filled);
  EXPECT_EQ(fx.om.find(cid)->filled_qty, 10u);
  EXPECT_EQ(fx.om.position(SYM),         10);
  EXPECT_EQ(fx.strategy.fills.size(),    2u);
}

TEST(OrderManager, CancelAckTransitionsToCancelled) {
  Fx fx;
  OrderId cid = fx.om.send_new(SYM, Side::Buy, 100, 10);
  fx.om.on_ack(cid, 9001);

  fx.om.send_cancel(cid);
  EXPECT_EQ(fx.om.find(cid)->state, OrderState::PendingCancel);
  ASSERT_EQ(fx.wire.sent_cancel.size(), 1u);
  EXPECT_EQ(fx.wire.sent_cancel[0].clord_id, cid);
  EXPECT_EQ(fx.wire.sent_cancel[0].exch_id,  9001u);

  fx.om.on_cancel_ack(9001);
  EXPECT_EQ(fx.om.find(cid)->state, OrderState::Cancelled);
  ASSERT_EQ(fx.strategy.cancels.size(), 1u);
  EXPECT_EQ(fx.strategy.cancels[0].clord_id, cid);
}

TEST(OrderManager, RejectTransitionsToRejectedAndCallsStrategy) {
  Fx fx;
  OrderId cid = fx.om.send_new(SYM, Side::Buy, 100, 10);

  fx.om.on_reject(cid, "price out of range");

  EXPECT_EQ(fx.om.find(cid)->state, OrderState::Rejected);
  ASSERT_EQ(fx.strategy.rejects.size(), 1u);
  EXPECT_EQ(fx.strategy.rejects[0].clord_id, cid);
  EXPECT_EQ(fx.strategy.rejects[0].reason,   "price out of range");
}

TEST(OrderManager, CancelPendingOrderIsNoop) {
  // Cancelling before ack: order not Live, wire should not get a cancel.
  // 未收到 ack 前撤单：订单非 Live 状态，线路不应收到撤单请求。
  Fx fx;
  OrderId cid = fx.om.send_new(SYM, Side::Buy, 100, 10);

  fx.om.send_cancel(cid);

  EXPECT_TRUE(fx.wire.sent_cancel.empty());
  EXPECT_EQ(fx.om.find(cid)->state, OrderState::Pending);
}

TEST(OrderManager, FillRaceAfterCancelStillUpdatesPosition) {
  // Classic race: cancel sent, fill arrives before cancel_ack.
  // 经典竞态：已发撤单，成交在撤单确认前到达。
  Fx fx;
  OrderId cid = fx.om.send_new(SYM, Side::Sell, 100, 5);
  fx.om.on_ack(cid, 9001);
  fx.om.send_cancel(cid); // PendingCancel

  fx.om.on_fill(9001, 5, 100); // fill wins the race

  EXPECT_EQ(fx.om.find(cid)->state, OrderState::Filled);
  EXPECT_EQ(fx.om.position(SYM),    -5); // sell → negative
  EXPECT_EQ(fx.strategy.fills.size(), 1u);
}

TEST(OrderManager, PositionNetsBuysAndSells) {
  Fx fx;
  OrderId buy  = fx.om.send_new(SYM, Side::Buy,  100, 10);
  OrderId sell = fx.om.send_new(SYM, Side::Sell, 101,  3);
  fx.om.on_ack(buy,  9001);
  fx.om.on_ack(sell, 9002);

  fx.om.on_fill(9001, 10, 100);
  fx.om.on_fill(9002,  3, 101);

  EXPECT_EQ(fx.om.position(SYM), 7); // 10 - 3
}

TEST(OrderManager, ClordIdsAreMonotonicallyIncreasing) {
  Fx fx;
  OrderId a = fx.om.send_new(SYM, Side::Buy, 100, 1);
  OrderId b = fx.om.send_new(SYM, Side::Buy, 100, 1);
  OrderId c = fx.om.send_new(SYM, Side::Buy, 100, 1);

  EXPECT_LT(a, b);
  EXPECT_LT(b, c);
}
