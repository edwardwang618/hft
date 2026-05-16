#include "hft/core/map_order_book.hpp"
#include "hft/core/types.hpp"
#include <functional>
#include <gtest/gtest.h>
#include <vector>

using namespace hft;
using namespace hft::core;
using OrderBook = MapOrderBook<>;

namespace {

Order mk(OrderId id, Side s, Price px, Qty q, Ts t = 0) {
  Order o{};
  o.id = id;
  o.side = s;
  o.price = px;
  o.qty = q;
  o.ts = t; // 如果 Order 没有 ts 字段, 删掉这一行
  return o;
}

struct Recorder {
  struct Fill {
    OrderId maker;
    OrderId taker;
    Price px;
    Qty qty;
  };
  std::vector<Fill> fills;
  void operator()(OrderId m, OrderId t, Price p, Qty q) {
    fills.push_back(Fill{m, t, p, q});
  }
};

} // namespace

TEST(OrderBook, EmptyBook) {
  OrderBook b;
  EXPECT_FALSE(b.best_bid().has_value());
  EXPECT_FALSE(b.best_ask().has_value());
}

TEST(OrderBook, SingleRestingOrder) {
  OrderBook b;
  b.add_limit(mk(1, Side::Buy, 10000, 5));
  ASSERT_TRUE(b.best_bid().has_value());
  EXPECT_EQ(*b.best_bid(), 10000);
  EXPECT_FALSE(b.best_ask().has_value());
}

TEST(OrderBook, NoCrossNoFill) {
  OrderBook b;
  Recorder r;
  b.set_trade_callback(std::ref(r));

  b.add_limit(mk(1, Side::Buy, 9999, 5));
  b.add_limit(mk(2, Side::Sell, 10001, 5));

  EXPECT_TRUE(r.fills.empty());
  EXPECT_EQ(*b.best_bid(), 9999);
  EXPECT_EQ(*b.best_ask(), 10001);
}

TEST(OrderBook, MarketableBuyPartialFill) {
  OrderBook b;
  Recorder r;
  b.set_trade_callback(std::ref(r));

  b.add_limit(mk(1, Side::Sell, 100, 10)); // rests
  b.add_limit(mk(2, Side::Buy, 101, 4));   // crosses, 4 filled @ 100

  ASSERT_EQ(r.fills.size(), 1u);
  EXPECT_EQ(r.fills[0].maker, 1u);
  EXPECT_EQ(r.fills[0].taker, 2u);
  EXPECT_EQ(r.fills[0].px, 100); // trade at resting price
  EXPECT_EQ(r.fills[0].qty, 4);

  EXPECT_EQ(*b.best_ask(), 100);          // 6 remaining on ask
  EXPECT_FALSE(b.best_bid().has_value()); // buy fully filled
}

TEST(OrderBook, FifoPriorityAtSamePrice) {
  OrderBook b;
  Recorder r;
  b.set_trade_callback(std::ref(r));

  b.add_limit(mk(1, Side::Sell, 100, 5, /*ts=*/1));
  b.add_limit(mk(2, Side::Sell, 100, 5, /*ts=*/2));
  b.add_limit(mk(9, Side::Buy, 100, 7)); // 5 from #1, 2 from #2

  ASSERT_EQ(r.fills.size(), 2u);
  EXPECT_EQ(r.fills[0].maker, 1u);
  EXPECT_EQ(r.fills[0].qty, 5);
  EXPECT_EQ(r.fills[1].maker, 2u);
  EXPECT_EQ(r.fills[1].qty, 2);
}

TEST(OrderBook, SweepMultipleLevels) {
  OrderBook b;
  Recorder r;
  b.set_trade_callback(std::ref(r));

  b.add_limit(mk(1, Side::Sell, 100, 5));
  b.add_limit(mk(2, Side::Sell, 101, 5));
  b.add_limit(mk(3, Side::Sell, 102, 5));

  b.add_limit(mk(9, Side::Buy, 102, 12)); // sweep 5+5+2

  ASSERT_EQ(r.fills.size(), 3u);
  EXPECT_EQ(r.fills[0].px, 100);
  EXPECT_EQ(r.fills[1].px, 101);
  EXPECT_EQ(r.fills[2].px, 102);
  EXPECT_EQ(r.fills[2].qty, 2);

  EXPECT_EQ(*b.best_ask(), 102); // 3 left at 102
}

TEST(OrderBook, CancelRemovesFromLevel) {
  OrderBook b;
  b.add_limit(mk(1, Side::Buy, 99, 5));
  b.add_limit(mk(2, Side::Buy, 99, 3));
  EXPECT_TRUE(b.cancel(1));
  EXPECT_TRUE(b.cancel(2));
  EXPECT_FALSE(b.best_bid().has_value()); // level gone too
}

TEST(OrderBook, CancelUnknownIdReturnsFalse) {
  OrderBook b;
  b.add_limit(mk(1, Side::Buy, 99, 5));
  EXPECT_FALSE(b.cancel(999));
}

TEST(OrderBook, RestedResidualAfterPartialCross) {
  OrderBook b;
  Recorder r;
  b.set_trade_callback(std::ref(r));

  b.add_limit(mk(1, Side::Sell, 100, 5));
  b.add_limit(mk(2, Side::Buy, 100, 12)); // 5 fills, 7 rests at 100 on bid

  ASSERT_EQ(r.fills.size(), 1u);
  EXPECT_EQ(*b.best_bid(), 100);
  EXPECT_FALSE(b.best_ask().has_value());
}

// ─────────────── reduce ───────────────

TEST(OrderBookReduce, ShrinksQtyAndLevelTotal) {
  OrderBook b;
  b.add_limit(mk(1, Side::Buy, 100, 10));
  b.add_limit(mk(2, Side::Buy, 100, 5)); // 同档排队后面

  ASSERT_TRUE(b.reduce(1, 3));
  EXPECT_EQ(b.qty_at(Side::Buy, 100), 3 + 5); // 10→3, 加上 id=2 的 5
  EXPECT_EQ(b.num_orders(), 2u);              // 还在 book 里
}

TEST(OrderBookReduce, PreservesTimePriority) {
  // reduce 之后, id=1 仍应在 id=2 前面成交.
  OrderBook b;
  b.add_limit(mk(1, Side::Sell, 200, 10, /*ts*/ 1));
  b.add_limit(mk(2, Side::Sell, 200, 10, /*ts*/ 2));
  ASSERT_TRUE(b.reduce(1, 4));

  // 用 taker 去扫, 记录 maker 顺序
  std::vector<OrderId> makers;
  b.set_trade_callback(
      [&](OrderId m, OrderId, Price, Qty) { makers.push_back(m); });
  b.add_limit(mk(99, Side::Buy, 200, 100)); // 全吃

  ASSERT_EQ(makers.size(), 2u);
  EXPECT_EQ(makers[0], 1u); // 小的先
  EXPECT_EQ(makers[1], 2u);
}

TEST(OrderBookReduce, RejectsGrowEqualZero) {
  OrderBook b;
  b.add_limit(mk(1, Side::Buy, 100, 10));
  EXPECT_FALSE(b.reduce(1, 10));           // 等于
  EXPECT_FALSE(b.reduce(1, 20));           // 大于
  EXPECT_FALSE(b.reduce(1, 0));            // 零
  EXPECT_EQ(b.qty_at(Side::Buy, 100), 10); // 状态不变
}

TEST(OrderBookReduce, UnknownIdFails) {
  OrderBook b;
  EXPECT_FALSE(b.reduce(999, 1));
}

// ─────────────── execute ───────────────

TEST(OrderBookExecute, PartialKeepsOrder) {
  OrderBook b;
  b.add_limit(mk(1, Side::Sell, 200, 10));
  ASSERT_TRUE(b.execute(1, 4));

  ASSERT_TRUE(b.best_ask().has_value());
  EXPECT_EQ(*b.best_ask(), 200);
  EXPECT_EQ(b.qty_at(Side::Sell, 200), 6);
  EXPECT_EQ(b.num_orders(), 1u);
}

TEST(OrderBookExecute, FullRemovesOrderAndLevel) {
  OrderBook b;
  b.add_limit(mk(1, Side::Sell, 200, 10));
  ASSERT_TRUE(b.execute(1, 10));

  EXPECT_FALSE(b.best_ask().has_value());
  EXPECT_EQ(b.qty_at(Side::Sell, 200), 0);
  EXPECT_EQ(b.num_orders(), 0u);
  EXPECT_EQ(b.num_ask_levels(), 0u);
  EXPECT_FALSE(b.cancel(1)); // 已经不在 index 里
}

TEST(OrderBookExecute, FullKeepsLevelIfOthersRest) {
  OrderBook b;
  b.add_limit(mk(1, Side::Sell, 200, 10));
  b.add_limit(mk(2, Side::Sell, 200, 5));
  ASSERT_TRUE(b.execute(1, 10));

  EXPECT_EQ(*b.best_ask(), 200);
  EXPECT_EQ(b.qty_at(Side::Sell, 200), 5);
  EXPECT_EQ(b.num_orders(), 1u);
  EXPECT_EQ(b.num_ask_levels(), 1u);
}

TEST(OrderBookExecute, OverfillRejects) {
  OrderBook b;
  b.add_limit(mk(1, Side::Sell, 200, 10));
  EXPECT_FALSE(b.execute(1, 11));
  EXPECT_EQ(*b.best_ask(), 200);
  EXPECT_EQ(b.qty_at(Side::Sell, 200), 10); // 状态不变
}

TEST(OrderBookExecute, ZeroRejects) {
  OrderBook b;
  b.add_limit(mk(1, Side::Sell, 200, 10));
  EXPECT_FALSE(b.execute(1, 0));
  EXPECT_EQ(b.qty_at(Side::Sell, 200), 10);
}

TEST(OrderBookExecute, UnknownIdFails) {
  OrderBook b;
  EXPECT_FALSE(b.execute(999, 1));
}

// ─────────────── 组合 ───────────────

TEST(OrderBookReduceExecute, ReduceThenExecuteToZero) {
  OrderBook b;
  b.add_limit(mk(1, Side::Buy, 100, 10));
  ASSERT_TRUE(b.reduce(1, 3));
  ASSERT_TRUE(b.execute(1, 3));
  EXPECT_FALSE(b.best_bid().has_value());
  EXPECT_EQ(b.num_orders(), 0u);
}
