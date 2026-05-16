#include <hft/strategy/simple_quoter.hpp>
#include <hft/core/array_order_book.hpp>
#include <gtest/gtest.h>
#include <vector>

using namespace hft;
using namespace hft::strategy;

namespace {

// ── Mock gateway ──────────────────────────────────────────────────────

struct MockGateway : IOrderGateway {
  struct NewOrder { OrderId id; md::SymbolId sym; Side side; Price px; Qty qty; };

  std::vector<NewOrder>  sent_new;
  std::vector<OrderId>   sent_cancel;
  OrderId next_id_{1};

  OrderId send_new(md::SymbolId sym, Side side, Price px, Qty qty) override {
    OrderId id = next_id_++;
    sent_new.push_back({id, sym, side, px, qty});
    return id;
  }
  void send_cancel(OrderId id) override { sent_cancel.push_back(id); }
  void send_replace(OrderId, Price, Qty) override {}
};

// ── Fixture ───────────────────────────────────────────────────────────

constexpr md::SymbolId SYM   = 7;
constexpr md::SymbolId OTHER = 9;
constexpr Qty          QTY   = 5;

struct Fx {
  MockGateway  gw;
  SimpleQuoter q{gw, {SYM, QTY}};

  void bbo(Price bid, Price ask, md::SymbolId sym = SYM) {
    q.on_bbo(sym, bid, QTY, ask, QTY);
  }
};

} // namespace

// ─────────────────────────────────────────────────────────────────────

// First BBO with both sides → quotes both sides immediately.
// 首次双边 BBO → 立即在两侧各挂一笔。
TEST(SimpleQuoter, InitialBboPostsBothSides) {
  Fx fx;
  fx.bbo(100, 101);

  ASSERT_EQ(fx.gw.sent_new.size(), 2u);
  EXPECT_EQ(fx.gw.sent_new[0].side, Side::Buy);
  EXPECT_EQ(fx.gw.sent_new[0].px,   100);
  EXPECT_EQ(fx.gw.sent_new[1].side, Side::Sell);
  EXPECT_EQ(fx.gw.sent_new[1].px,   101);
  EXPECT_EQ(fx.gw.sent_new[0].qty,  QTY);
  EXPECT_EQ(fx.gw.sent_new[1].qty,  QTY);
  EXPECT_TRUE(fx.gw.sent_cancel.empty());

  EXPECT_NE(fx.q.bid_id(), 0u);
  EXPECT_NE(fx.q.ask_id(), 0u);
}

// One-sided book (ask==0) → only bid is quoted.
// 单边行情（无卖盘）→ 只报买单。
TEST(SimpleQuoter, OneSidedAskZeroQuotesBidOnly) {
  Fx fx;
  fx.bbo(100, 0);

  ASSERT_EQ(fx.gw.sent_new.size(), 1u);
  EXPECT_EQ(fx.gw.sent_new[0].side, Side::Buy);
  EXPECT_EQ(fx.q.bid_id(), fx.gw.sent_new[0].id);
  EXPECT_EQ(fx.q.ask_id(), 0u);
}

// One-sided book (bid==0) → only ask is quoted.
TEST(SimpleQuoter, OneSidedBidZeroQuotesAskOnly) {
  Fx fx;
  fx.bbo(0, 101);

  ASSERT_EQ(fx.gw.sent_new.size(), 1u);
  EXPECT_EQ(fx.gw.sent_new[0].side, Side::Sell);
  EXPECT_EQ(fx.q.ask_id(), fx.gw.sent_new[0].id);
  EXPECT_EQ(fx.q.bid_id(), 0u);
}

// BBO price changes → cancel old order, submit new one at new price.
// BBO 价格变动 → 撤旧单，在新价格重新报价。
TEST(SimpleQuoter, BboBidMoveCancelsAndRequotes) {
  Fx fx;
  fx.bbo(100, 101);

  OrderId old_bid = fx.q.bid_id();
  fx.gw.sent_new.clear();

  fx.bbo(99, 101); // bid moved down

  ASSERT_EQ(fx.gw.sent_cancel.size(), 1u);
  EXPECT_EQ(fx.gw.sent_cancel[0], old_bid);

  ASSERT_EQ(fx.gw.sent_new.size(), 1u);
  EXPECT_EQ(fx.gw.sent_new[0].side, Side::Buy);
  EXPECT_EQ(fx.gw.sent_new[0].px,   99);
  EXPECT_EQ(fx.q.bid_px(), 99);
}

TEST(SimpleQuoter, BboAskMoveCancelsAndRequotes) {
  Fx fx;
  fx.bbo(100, 101);

  OrderId old_ask = fx.q.ask_id();
  fx.gw.sent_new.clear();

  fx.bbo(100, 102); // ask moved up

  ASSERT_EQ(fx.gw.sent_cancel.size(), 1u);
  EXPECT_EQ(fx.gw.sent_cancel[0], old_ask);

  ASSERT_EQ(fx.gw.sent_new.size(), 1u);
  EXPECT_EQ(fx.gw.sent_new[0].side, Side::Sell);
  EXPECT_EQ(fx.gw.sent_new[0].px,   102);
}

// Same BBO price repeated → no cancel, no new order.
// 相同 BBO 重复到达 → 不撤单，不重新报价。
TEST(SimpleQuoter, SamePriceNoExtraOrders) {
  Fx fx;
  fx.bbo(100, 101);
  fx.gw.sent_new.clear();

  fx.bbo(100, 101); // same prices

  EXPECT_TRUE(fx.gw.sent_cancel.empty());
  EXPECT_TRUE(fx.gw.sent_new.empty());
}

// BBO for a different symbol is ignored entirely.
// 其他品种的 BBO 更新应被忽略。
TEST(SimpleQuoter, DifferentSymbolIgnored) {
  Fx fx;
  fx.bbo(200, 201, OTHER);

  EXPECT_TRUE(fx.gw.sent_new.empty());
  EXPECT_EQ(fx.q.bid_id(), 0u);
  EXPECT_EQ(fx.q.ask_id(), 0u);
}

// Both sides move simultaneously → cancel both, requote both.
// 两侧同时变动 → 撤双边旧单，重新双边报价。
TEST(SimpleQuoter, BothSidesMoveRequotesBoth) {
  Fx fx;
  fx.bbo(100, 101);

  OrderId old_bid = fx.q.bid_id();
  OrderId old_ask = fx.q.ask_id();
  fx.gw.sent_new.clear();

  fx.bbo(99, 102); // both moved

  ASSERT_EQ(fx.gw.sent_cancel.size(), 2u);
  // Cancel order is unspecified, check both are cancelled
  auto &c = fx.gw.sent_cancel;
  EXPECT_TRUE(std::find(c.begin(), c.end(), old_bid) != c.end());
  EXPECT_TRUE(std::find(c.begin(), c.end(), old_ask) != c.end());

  ASSERT_EQ(fx.gw.sent_new.size(), 2u);
  EXPECT_EQ(fx.q.bid_px(), 99);
  EXPECT_EQ(fx.q.ask_px(), 102);
}

// ── Integration: SimpleQuoter wired to a real ArrayOrderBook ─────────
// SimpleQuoter 接入真实 ArrayOrderBook 的集成测试。

TEST(SimpleQuoter, WiredToArrayOrderBook) {
  using namespace hft::core;

  MockGateway  gw;
  SimpleQuoter q{gw, {SYM, 3}};

  constexpr Price MIN_PX = 1;
  constexpr uint32_t TICKS = 200;
  ArrayOrderBook<SimpleQuoter> book{q, SYM, MIN_PX, TICKS, 1};

  // Add best bid and ask → BBO fires → quoter responds
  book.apply(md::Add{0, SYM, 1, Side::Buy,  100, 10});
  book.apply(md::Add{0, SYM, 2, Side::Sell, 101,  8});

  ASSERT_EQ(gw.sent_new.size(), 2u);
  EXPECT_EQ(gw.sent_new[0].px, 100);
  EXPECT_EQ(gw.sent_new[1].px, 101);

  // BBO bid improves → quoter cancels old bid, requotes at 102
  gw.sent_new.clear();
  book.apply(md::Add{0, SYM, 3, Side::Buy, 102, 5}); // new best bid

  ASSERT_EQ(gw.sent_cancel.size(), 1u);
  ASSERT_EQ(gw.sent_new.size(), 1u);
  EXPECT_EQ(gw.sent_new[0].side, Side::Buy);
  EXPECT_EQ(gw.sent_new[0].px,   102);
}
