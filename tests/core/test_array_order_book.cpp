#include "hft/core/types.hpp"
#include <hft/core/array_order_book.hpp>
#include <gtest/gtest.h>
#include <functional>
#include <vector>

using namespace hft;
using namespace hft::core;

namespace {

// Price range: 1 – 20000, tick = 1.
ArrayOrderBook<> make_book() { return ArrayOrderBook<>(1, 20000, 1); }

Order mk(OrderId id, Side s, Price px, Qty q, Ts t = 0) {
  Order o{};
  o.id    = id;
  o.side  = s;
  o.price = px;
  o.qty   = q;
  o.ts    = t;
  return o;
}

struct Recorder {
  struct Fill { OrderId maker, taker; Price px; Qty qty; };
  std::vector<Fill> fills;
  void operator()(OrderId m, OrderId t, Price p, Qty q) {
    fills.push_back({m, t, p, q});
  }
};

} // namespace

TEST(ArrayOrderBook, EmptyBook) {
  auto b = make_book();
  EXPECT_FALSE(b.best_bid().has_value());
  EXPECT_FALSE(b.best_ask().has_value());
}

TEST(ArrayOrderBook, SingleRestingOrder) {
  auto b = make_book();
  b.add_limit(mk(1, Side::Buy, 10000, 5));
  ASSERT_TRUE(b.best_bid().has_value());
  EXPECT_EQ(*b.best_bid(), 10000);
  EXPECT_FALSE(b.best_ask().has_value());
}

TEST(ArrayOrderBook, NoCrossNoFill) {
  auto b = make_book();
  Recorder r;
  b.set_trade_callback(std::ref(r));

  b.add_limit(mk(1, Side::Buy,   999, 5));
  b.add_limit(mk(2, Side::Sell, 1001, 5));

  EXPECT_TRUE(r.fills.empty());
  EXPECT_EQ(*b.best_bid(),   999);
  EXPECT_EQ(*b.best_ask(), 1001);
}

TEST(ArrayOrderBook, MarketableBuyPartialFill) {
  auto b = make_book();
  Recorder r;
  b.set_trade_callback(std::ref(r));

  b.add_limit(mk(1, Side::Sell, 100, 10));
  b.add_limit(mk(2, Side::Buy,  101,  4));

  ASSERT_EQ(r.fills.size(), 1u);
  EXPECT_EQ(r.fills[0].maker, 1u);
  EXPECT_EQ(r.fills[0].taker, 2u);
  EXPECT_EQ(r.fills[0].px,    100);
  EXPECT_EQ(r.fills[0].qty,   4);

  EXPECT_EQ(*b.best_ask(), 100);
  EXPECT_FALSE(b.best_bid().has_value());
}

TEST(ArrayOrderBook, FifoPriorityAtSamePrice) {
  auto b = make_book();
  Recorder r;
  b.set_trade_callback(std::ref(r));

  b.add_limit(mk(1, Side::Sell, 100, 5, /*ts=*/1));
  b.add_limit(mk(2, Side::Sell, 100, 5, /*ts=*/2));
  b.add_limit(mk(9, Side::Buy,  100, 7));

  ASSERT_EQ(r.fills.size(), 2u);
  EXPECT_EQ(r.fills[0].maker, 1u);
  EXPECT_EQ(r.fills[0].qty,   5);
  EXPECT_EQ(r.fills[1].maker, 2u);
  EXPECT_EQ(r.fills[1].qty,   2);
}

TEST(ArrayOrderBook, SweepMultipleLevels) {
  auto b = make_book();
  Recorder r;
  b.set_trade_callback(std::ref(r));

  b.add_limit(mk(1, Side::Sell, 100, 5));
  b.add_limit(mk(2, Side::Sell, 101, 5));
  b.add_limit(mk(3, Side::Sell, 102, 5));
  b.add_limit(mk(9, Side::Buy,  102, 12));

  ASSERT_EQ(r.fills.size(), 3u);
  EXPECT_EQ(r.fills[0].px,  100);
  EXPECT_EQ(r.fills[1].px,  101);
  EXPECT_EQ(r.fills[2].px,  102);
  EXPECT_EQ(r.fills[2].qty, 2);

  EXPECT_EQ(*b.best_ask(), 102);
}

TEST(ArrayOrderBook, CancelRemovesFromLevel) {
  auto b = make_book();
  b.add_limit(mk(1, Side::Buy, 99, 5));
  b.add_limit(mk(2, Side::Buy, 99, 3));
  EXPECT_TRUE(b.cancel(1));
  EXPECT_TRUE(b.cancel(2));
  EXPECT_FALSE(b.best_bid().has_value());
}

TEST(ArrayOrderBook, CancelUnknownIdReturnsFalse) {
  auto b = make_book();
  b.add_limit(mk(1, Side::Buy, 99, 5));
  EXPECT_FALSE(b.cancel(999));
}

TEST(ArrayOrderBook, RestedResidualAfterPartialCross) {
  auto b = make_book();
  Recorder r;
  b.set_trade_callback(std::ref(r));

  b.add_limit(mk(1, Side::Sell, 100,  5));
  b.add_limit(mk(2, Side::Buy,  100, 12));

  ASSERT_EQ(r.fills.size(), 1u);
  EXPECT_EQ(*b.best_bid(), 100);
  EXPECT_FALSE(b.best_ask().has_value());
}

// ─────────────── reduce ───────────────

TEST(ArrayOrderBookReduce, ShrinksQtyAndLevelTotal) {
  auto b = make_book();
  b.add_limit(mk(1, Side::Buy, 100, 10));
  b.add_limit(mk(2, Side::Buy, 100,  5));

  ASSERT_TRUE(b.reduce(1, 3));
  EXPECT_EQ(b.qty_at(Side::Buy, 100), 3 + 5);
  EXPECT_EQ(b.num_orders(), 2u);
}

TEST(ArrayOrderBookReduce, PreservesTimePriority) {
  auto b = make_book();
  b.add_limit(mk(1, Side::Sell, 200, 10, /*ts=*/1));
  b.add_limit(mk(2, Side::Sell, 200, 10, /*ts=*/2));
  ASSERT_TRUE(b.reduce(1, 4));

  std::vector<OrderId> makers;
  b.set_trade_callback([&](OrderId m, OrderId, Price, Qty){ makers.push_back(m); });
  b.add_limit(mk(99, Side::Buy, 200, 100));

  ASSERT_EQ(makers.size(), 2u);
  EXPECT_EQ(makers[0], 1u);
  EXPECT_EQ(makers[1], 2u);
}

TEST(ArrayOrderBookReduce, RejectsGrowEqualZero) {
  auto b = make_book();
  b.add_limit(mk(1, Side::Buy, 100, 10));
  EXPECT_FALSE(b.reduce(1, 10));
  EXPECT_FALSE(b.reduce(1, 20));
  EXPECT_FALSE(b.reduce(1,  0));
  EXPECT_EQ(b.qty_at(Side::Buy, 100), 10);
}

TEST(ArrayOrderBookReduce, UnknownIdFails) {
  auto b = make_book();
  EXPECT_FALSE(b.reduce(999, 1));
}

// ─────────────── execute ───────────────

TEST(ArrayOrderBookExecute, PartialKeepsOrder) {
  auto b = make_book();
  b.add_limit(mk(1, Side::Sell, 200, 10));
  ASSERT_TRUE(b.execute(1, 4));

  EXPECT_EQ(*b.best_ask(), 200);
  EXPECT_EQ(b.qty_at(Side::Sell, 200), 6);
  EXPECT_EQ(b.num_orders(), 1u);
}

TEST(ArrayOrderBookExecute, FullRemovesOrderAndLevel) {
  auto b = make_book();
  b.add_limit(mk(1, Side::Sell, 200, 10));
  ASSERT_TRUE(b.execute(1, 10));

  EXPECT_FALSE(b.best_ask().has_value());
  EXPECT_EQ(b.qty_at(Side::Sell, 200), 0);
  EXPECT_EQ(b.num_orders(), 0u);
  EXPECT_EQ(b.num_ask_levels(), 0u);
  EXPECT_FALSE(b.cancel(1));
}

TEST(ArrayOrderBookExecute, FullKeepsLevelIfOthersRest) {
  auto b = make_book();
  b.add_limit(mk(1, Side::Sell, 200, 10));
  b.add_limit(mk(2, Side::Sell, 200,  5));
  ASSERT_TRUE(b.execute(1, 10));

  EXPECT_EQ(*b.best_ask(), 200);
  EXPECT_EQ(b.qty_at(Side::Sell, 200), 5);
  EXPECT_EQ(b.num_orders(), 1u);
  EXPECT_EQ(b.num_ask_levels(), 1u);
}

TEST(ArrayOrderBookExecute, OverfillRejects) {
  auto b = make_book();
  b.add_limit(mk(1, Side::Sell, 200, 10));
  EXPECT_FALSE(b.execute(1, 11));
  EXPECT_EQ(b.qty_at(Side::Sell, 200), 10);
}

TEST(ArrayOrderBookExecute, ZeroRejects) {
  auto b = make_book();
  b.add_limit(mk(1, Side::Sell, 200, 10));
  EXPECT_FALSE(b.execute(1, 0));
  EXPECT_EQ(b.qty_at(Side::Sell, 200), 10);
}

TEST(ArrayOrderBookExecute, UnknownIdFails) {
  auto b = make_book();
  EXPECT_FALSE(b.execute(999, 1));
}

// ─────────────── combined ───────────────

TEST(ArrayOrderBookReduceExecute, ReduceThenExecuteToZero) {
  auto b = make_book();
  b.add_limit(mk(1, Side::Buy, 100, 10));
  ASSERT_TRUE(b.reduce(1, 3));
  ASSERT_TRUE(b.execute(1, 3));
  EXPECT_FALSE(b.best_bid().has_value());
  EXPECT_EQ(b.num_orders(), 0u);
}

// ─────────────── array-specific ───────────────

// Verifies that tombstoned orders in the middle of a queue don't break FIFO
// matching after a cancel.
TEST(ArrayOrderBook, CancelMiddleOrderFifoPreserved) {
  auto b = make_book();
  Recorder r;
  b.set_trade_callback(std::ref(r));

  b.add_limit(mk(1, Side::Sell, 100, 5));
  b.add_limit(mk(2, Side::Sell, 100, 5));
  b.add_limit(mk(3, Side::Sell, 100, 5));
  ASSERT_TRUE(b.cancel(2));  // tombstone in middle

  b.add_limit(mk(9, Side::Buy, 100, 8));  // 5 from #1, 3 from #3

  ASSERT_EQ(r.fills.size(), 2u);
  EXPECT_EQ(r.fills[0].maker, 1u);
  EXPECT_EQ(r.fills[0].qty,   5);
  EXPECT_EQ(r.fills[1].maker, 3u);
  EXPECT_EQ(r.fills[1].qty,   3);
  EXPECT_EQ(b.qty_at(Side::Sell, 100), 2);
}

// Verifies compaction: fill 33+ orders at a level so head > 32 and > half the
// queue, then cancel the remaining order — the level must be correctly emptied.
TEST(ArrayOrderBook, CompactionKeepsStateConsistent) {
  auto b = make_book();

  // Add 34 sell orders + 1 sentinel at the same price.
  for (OrderId id = 1; id <= 34; ++id)
    b.add_limit(mk(id, Side::Sell, 100, 1));
  b.add_limit(mk(99, Side::Sell, 100, 10));  // sentinel, will survive compaction

  // A large buy sweeps the first 34 orders, triggering compaction.
  b.add_limit(mk(200, Side::Buy, 100, 34));

  EXPECT_EQ(b.qty_at(Side::Sell, 100), 10);
  EXPECT_EQ(*b.best_ask(), 100);

  // The sentinel is still cancellable after compaction updated its locator.
  ASSERT_TRUE(b.cancel(99));
  EXPECT_FALSE(b.best_ask().has_value());
  EXPECT_EQ(b.num_orders(), 0u);
}

TEST(ArrayOrderBook, OutOfRangePriceRejected) {
  // Book covers prices 1–20000.
  auto b = make_book();
  EXPECT_FALSE(b.add_limit(mk(1, Side::Buy,      0, 5)));  // below min
  EXPECT_FALSE(b.add_limit(mk(2, Side::Buy,  20001, 5)));  // above max
  EXPECT_FALSE(b.add_limit(mk(3, Side::Sell,    -1, 5)));  // negative
  // Book must remain empty.
  EXPECT_FALSE(b.best_bid().has_value());
  EXPECT_FALSE(b.best_ask().has_value());
  EXPECT_EQ(b.num_orders(), 0u);
}
