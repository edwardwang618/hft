#include <hft/core/array_order_book.hpp>
#include <hft/pipeline/book_builder.hpp>
#include <hft/strategy/i_order_gateway.hpp>
#include <hft/strategy/strategy_base.hpp>
#include <gtest/gtest.h>
#include <vector>

using namespace hft;
using namespace hft::core;
using namespace hft::md;
using namespace hft::strategy;

namespace {

// ─── Mock gateway ────────────────────────────────────────────────────────

struct MockGateway : IOrderGateway {
  struct NewOrder { OrderId id; SymbolId sym; Side side; Price px; Qty qty; };
  struct CancelReq { OrderId id; };

  std::vector<NewOrder>  sent_new;
  std::vector<CancelReq> sent_cancel;
  OrderId next_id_{1};

  OrderId send_new(SymbolId sym, Side side, Price px, Qty qty) override {
    OrderId id = next_id_++;
    sent_new.push_back({id, sym, side, px, qty});
    return id;
  }
  void send_cancel(OrderId id) override { sent_cancel.push_back({id}); }
  void send_replace(OrderId, Price, Qty) override {}
};

// ─── Recording listener (no gateway needed for book-callback tests) ──────

struct RecordingListener {
  struct BboRecord   { SymbolId sym; Price bid, ask; Qty bq, aq; };
  struct DepthRecord { SymbolId sym; };
  struct TradeRecord { SymbolId sym; Price px; Qty qty; TradeId trade_id; };

  std::vector<BboRecord>   bbos;
  std::vector<DepthRecord> depths;
  std::vector<TradeRecord> trades;

  void on_bbo(SymbolId sym, Price bid, Qty bq, Price ask, Qty aq) noexcept {
    bbos.push_back({sym, bid, ask, bq, aq});
  }
  template <class Book>
  void on_depth(SymbolId sym, const Book &) noexcept {
    depths.push_back({sym});
  }
  void on_trade(SymbolId sym, Price px, Qty qty, TradeId tid) noexcept {
    trades.push_back({sym, px, qty, tid});
  }
};

// ─── Example concrete strategy ───────────────────────────────────────────

// On every BBO update, auto-quotes a passive bid 1 tick below the best ask.
struct QuoteStrategy : StrategyBase {
  explicit QuoteStrategy(IOrderGateway &gw) : StrategyBase(gw) {}

  SymbolId last_sym{0};
  Price last_bid{0}, last_ask{0};
  Qty   last_bq{0},  last_aq{0};

  void on_bbo(SymbolId sym, Price bid, Qty bq, Price ask, Qty aq) noexcept override {
    last_sym = sym; last_bid = bid; last_bq = bq; last_ask = ask; last_aq = aq;
    if (ask > 0)
      send_new(sym, Side::Buy, ask - 1, 1);
  }
};

// ─── Helpers ─────────────────────────────────────────────────────────────

constexpr SymbolId SYM = 1;
constexpr Price MIN_PX = 1;
constexpr uint32_t TICKS = 20000;

using RecBook = ArrayOrderBook<RecordingListener>;

RecBook make_rec_book(RecordingListener &l) {
  return RecBook(l, SYM, MIN_PX, TICKS, 1);
}

Add make_add(OrderId id, Side s, Price px, Qty q, Ts ts = 0) {
  return Add{ts, SYM, id, s, px, q};
}

} // namespace

// ═══════════════════════════════════════════════════════════════════════
// BBO callback tests
// ═══════════════════════════════════════════════════════════════════════

TEST(StrategyListener, AddBidFiresBbo) {
  RecordingListener l;
  auto b = make_rec_book(l);

  b.apply(make_add(1, Side::Buy, 100, 10));

  ASSERT_EQ(l.bbos.size(), 1u);
  EXPECT_EQ(l.bbos[0].sym,  SYM);
  EXPECT_EQ(l.bbos[0].bid,  100);
  EXPECT_EQ(l.bbos[0].bq,   10);
  EXPECT_EQ(l.bbos[0].ask,  0);   // no ask yet
  EXPECT_TRUE(l.depths.empty());
}

TEST(StrategyListener, AddAskFiresBbo) {
  RecordingListener l;
  auto b = make_rec_book(l);

  b.apply(make_add(1, Side::Sell, 105, 5));

  ASSERT_EQ(l.bbos.size(), 1u);
  EXPECT_EQ(l.bbos[0].ask, 105);
  EXPECT_EQ(l.bbos[0].aq,  5);
  EXPECT_EQ(l.bbos[0].bid, 0);
}

// Adding a second order behind the BBO changes depth but not BBO.
TEST(StrategyListener, AddBehindBboFiresDepthNotBbo) {
  RecordingListener l;
  auto b = make_rec_book(l);

  b.apply(make_add(1, Side::Buy, 100, 10));  // sets BBO → on_bbo
  l.bbos.clear();

  b.apply(make_add(2, Side::Buy, 99, 5));    // behind BBO → on_depth
  EXPECT_TRUE(l.bbos.empty());
  ASSERT_EQ(l.depths.size(), 1u);
}

// Adding qty at the same BBO level changes the BBO qty → on_bbo.
TEST(StrategyListener, AddAtBboLevelFiresBbo) {
  RecordingListener l;
  auto b = make_rec_book(l);

  b.apply(make_add(1, Side::Buy, 100, 10));
  l.bbos.clear(); l.depths.clear();

  b.apply(make_add(2, Side::Buy, 100, 5)); // same level, qty changes
  ASSERT_EQ(l.bbos.size(), 1u);
  EXPECT_EQ(l.bbos[0].bq, 15);
  EXPECT_TRUE(l.depths.empty());
}

TEST(StrategyListener, CancelBboFiresBbo) {
  RecordingListener l;
  auto b = make_rec_book(l);
  b.apply(make_add(1, Side::Sell, 105, 5));
  l.bbos.clear();

  b.apply(Cancel{0, SYM, 1});

  ASSERT_EQ(l.bbos.size(), 1u);
  EXPECT_EQ(l.bbos[0].ask, 0); // ask side empty after cancel
}

TEST(StrategyListener, CancelBehindBboFiresDepth) {
  RecordingListener l;
  auto b = make_rec_book(l);
  b.apply(make_add(1, Side::Sell, 105, 5));
  b.apply(make_add(2, Side::Sell, 110, 3)); // behind BBO
  l.bbos.clear(); l.depths.clear();

  b.apply(Cancel{0, SYM, 2}); // cancel non-BBO order
  EXPECT_TRUE(l.bbos.empty());
  ASSERT_EQ(l.depths.size(), 1u);
}

TEST(StrategyListener, ExecFiresTradeAndBbo) {
  RecordingListener l;
  auto b = make_rec_book(l);
  b.apply(make_add(1, Side::Sell, 100, 10));
  l.bbos.clear();

  b.apply(Exec{0, SYM, 1, 10, 100, /*trade_id=*/42});

  ASSERT_EQ(l.trades.size(), 1u);
  EXPECT_EQ(l.trades[0].px,       100);
  EXPECT_EQ(l.trades[0].qty,      10);
  EXPECT_EQ(l.trades[0].trade_id, 42u);

  // Full execution removes the BBO ask → on_bbo fires
  ASSERT_EQ(l.bbos.size(), 1u);
  EXPECT_EQ(l.bbos[0].ask, 0);
}

TEST(StrategyListener, ExecPartialFiresTradeAndBbo) {
  RecordingListener l;
  auto b = make_rec_book(l);
  b.apply(make_add(1, Side::Sell, 100, 10));
  l.bbos.clear();

  // Partial exec: qty changes at BBO → on_bbo
  b.apply(Exec{0, SYM, 1, 4, 100, 7});

  ASSERT_EQ(l.trades.size(), 1u);
  ASSERT_EQ(l.bbos.size(), 1u);
  EXPECT_EQ(l.bbos[0].aq, 6);
}

TEST(StrategyListener, TradeMdEventFiresTradeOnlyNoBook) {
  RecordingListener l;
  auto b = make_rec_book(l);
  b.apply(make_add(1, Side::Buy, 100, 10));
  l.bbos.clear(); l.depths.clear();

  b.apply(Trade{0, SYM, 100, 3, 99}); // non-displayed trade
  ASSERT_EQ(l.trades.size(), 1u);
  EXPECT_TRUE(l.bbos.empty());
  EXPECT_TRUE(l.depths.empty());
}

TEST(StrategyListener, ClearFiresBboWithZeros) {
  RecordingListener l;
  auto b = make_rec_book(l);
  b.apply(make_add(1, Side::Buy,  100, 5));
  b.apply(make_add(2, Side::Sell, 105, 5));
  l.bbos.clear();

  b.apply(Clear{0, SYM});

  ASSERT_EQ(l.bbos.size(), 1u);
  EXPECT_EQ(l.bbos[0].bid, 0);
  EXPECT_EQ(l.bbos[0].ask, 0);
  EXPECT_EQ(b.num_orders(), 0u);
}

TEST(StrategyListener, ReduceBboFiresBbo) {
  RecordingListener l;
  auto b = make_rec_book(l);
  b.apply(make_add(1, Side::Buy, 100, 10));
  l.bbos.clear();

  b.apply(Reduce{0, SYM, 1, 3}); // reduce to qty=3 → BBO qty changes
  ASSERT_EQ(l.bbos.size(), 1u);
  EXPECT_EQ(l.bbos[0].bq, 3);
}

TEST(StrategyListener, ReduceBehindBboFiresDepth) {
  RecordingListener l;
  auto b = make_rec_book(l);
  b.apply(make_add(1, Side::Buy, 100, 10));
  b.apply(make_add(2, Side::Buy,  99, 10)); // behind BBO
  l.bbos.clear(); l.depths.clear();

  b.apply(Reduce{0, SYM, 2, 3});
  EXPECT_TRUE(l.bbos.empty());
  ASSERT_EQ(l.depths.size(), 1u);
}

// ═══════════════════════════════════════════════════════════════════════
// StrategyBase + IOrderGateway tests
// ═══════════════════════════════════════════════════════════════════════

TEST(StrategyBase, GatewayReceivesOrderFromOnBbo) {
  MockGateway gw;
  QuoteStrategy strategy(gw);

  ArrayOrderBook<QuoteStrategy> b(strategy, SYM, MIN_PX, TICKS, 1);
  b.apply(make_add(1, Side::Sell, 105, 5));

  EXPECT_EQ(strategy.last_ask, 105);
  EXPECT_EQ(strategy.last_aq,  5);

  // QuoteStrategy.on_bbo sent a bid 1 tick below the ask
  ASSERT_EQ(gw.sent_new.size(), 1u);
  EXPECT_EQ(gw.sent_new[0].sym,  SYM);
  EXPECT_EQ(gw.sent_new[0].side, Side::Buy);
  EXPECT_EQ(gw.sent_new[0].px,   104);
  EXPECT_EQ(gw.sent_new[0].qty,  1u);
}

// ═══════════════════════════════════════════════════════════════════════
// StrategyBookBuilder tests
// ═══════════════════════════════════════════════════════════════════════

TEST(StrategyBookBuilder, RoutesEventsToCorrectBook) {
  RecordingListener l;

  pipeline::BookBuilder<pipeline::NullNext, RecBook> builder(
    [&](SymbolId sym) {
      return RecBook(l, sym, MIN_PX, TICKS, 1);
    });

  builder.on_md(1, Add{0, 1, 10, Side::Buy, 100, 5});
  builder.on_md(2, Add{0, 2, 20, Side::Buy, 200, 3});

  ASSERT_NE(builder.book(1), nullptr);
  ASSERT_NE(builder.book(2), nullptr);

  EXPECT_EQ(builder.book(1)->best_bid(), 100);
  EXPECT_EQ(builder.book(2)->best_bid(), 200);

  // Two BBO callbacks fired (one per Add)
  EXPECT_EQ(l.bbos.size(), 2u);
}

TEST(StrategyBookBuilder, UnknownSymbolReturnsNullptr) {
  RecordingListener l;
  pipeline::BookBuilder<pipeline::NullNext, RecBook> builder(
    [&](SymbolId sym) { return RecBook(l, sym, MIN_PX, TICKS, 1); });

  EXPECT_EQ(builder.book(999), nullptr);
}

TEST(StrategyBookBuilder, ClearIsolatesSymbols) {
  RecordingListener l;
  pipeline::BookBuilder<pipeline::NullNext, RecBook> builder(
    [&](SymbolId sym) { return RecBook(l, sym, MIN_PX, TICKS, 1); });

  builder.on_md(1, Add{0, 1, 10, Side::Buy, 100, 5});
  builder.on_md(2, Add{0, 2, 20, Side::Buy, 200, 3});
  builder.on_md(3, Clear{0, 1}); // clear sym 1 only

  // Clear removes the book entry entirely; sym 2 is unaffected.
  EXPECT_EQ(builder.book(1), nullptr);
  ASSERT_NE(builder.book(2), nullptr);
  EXPECT_EQ(*builder.book(2)->best_bid(), 200);
}
