#include <hft/risk/rate_limiter.hpp>
#include <hft/risk/risk_gateway.hpp>
#include <hft/risk/risk_limits.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <thread>

using namespace hft;
using namespace hft::risk;

// ── mock gateway
// ──────────────────────────────────────────────────────────────

struct MockGateway : strategy::IOrderGateway {
  OrderId send_new(md::SymbolId, Side, Price, Qty) override {
    return next_id_++;
  }
  void send_cancel(OrderId) override { ++cancels; }
  void send_replace(OrderId, Price, Qty) override { ++replaces; }

  OrderId next_id_{1};
  int cancels{0};
  int replaces{0};
};

// ── helpers
// ───────────────────────────────────────────────────────────────────

static RiskGateway make_gw(MockGateway &mock, RiskLimits limits) {
  return RiskGateway(mock, limits);
}

// ── kill switch
// ───────────────────────────────────────────────────────────────

TEST(RiskGateway, KillSwitchBlocksSendNew) {
  MockGateway mock;
  RiskLimits lim{};
  RiskGateway gw(mock, lim);

  gw.kill();
  EXPECT_EQ(gw.send_new(1, Side::Buy, 10000, 10), kInvalidOrderId);
  EXPECT_EQ(mock.next_id_, 1u); // nothing forwarded
  EXPECT_EQ(gw.rejected_kill(), 1u);
}

TEST(RiskGateway, KillSwitchBlocksCancel) {
  MockGateway mock;
  RiskGateway gw(mock, {});
  gw.kill();
  gw.send_cancel(1);
  EXPECT_EQ(mock.cancels, 0);
  EXPECT_EQ(gw.rejected_kill(), 1u);
}

TEST(RiskGateway, UnkillRestoresForwarding) {
  MockGateway mock;
  RiskGateway gw(mock, {});
  gw.kill();
  gw.unkill();
  EXPECT_NE(gw.send_new(1, Side::Buy, 10000, 10), kInvalidOrderId);
  EXPECT_EQ(mock.next_id_, 2u);
}

// ── max order qty
// ─────────────────────────────────────────────────────────────

TEST(RiskGateway, MaxQtyRejectsOversizedOrder) {
  MockGateway mock;
  RiskLimits lim{};
  lim.max_order_qty = 100;
  RiskGateway gw(mock, lim);

  EXPECT_EQ(gw.send_new(1, Side::Buy, 10000, 101), kInvalidOrderId);
  EXPECT_EQ(gw.rejected_qty(), 1u);
  EXPECT_EQ(mock.next_id_, 1u);
}

TEST(RiskGateway, MaxQtyAllowsExactLimit) {
  MockGateway mock;
  RiskLimits lim{};
  lim.max_order_qty = 100;
  RiskGateway gw(mock, lim);

  EXPECT_NE(gw.send_new(1, Side::Buy, 10000, 100), kInvalidOrderId);
  EXPECT_EQ(gw.rejected_qty(), 0u);
}

// ── max notional
// ──────────────────────────────────────────────────────────────

TEST(RiskGateway, MaxNotionalRejectsLargeOrder) {
  MockGateway mock;
  RiskLimits lim{};
  // max notional = 1000 * kPriceScale (= price 1000.0000, qty 1)
  lim.max_order_notional = 1000 * kPriceScale;
  RiskGateway gw(mock, lim);

  // px=1000*kPriceScale, qty=2 → notional = 2000*kPriceScale > limit
  EXPECT_EQ(gw.send_new(1, Side::Buy, 1000 * kPriceScale, 2), kInvalidOrderId);
  EXPECT_EQ(gw.rejected_notional(), 1u);
}

TEST(RiskGateway, MaxNotionalAllowsExactLimit) {
  MockGateway mock;
  RiskLimits lim{};
  lim.max_order_notional = 1000 * kPriceScale;
  RiskGateway gw(mock, lim);

  EXPECT_NE(gw.send_new(1, Side::Buy, 1000 * kPriceScale, 1), kInvalidOrderId);
  EXPECT_EQ(gw.rejected_notional(), 0u);
}

// ── rate limiter
// ──────────────────────────────────────────────────────────────

TEST(RateLimiter, AllowsUpToMaxRate) {
  RateLimiter rl(5);
  for (int i = 0; i < 5; ++i)
    EXPECT_TRUE(rl.try_acquire()) << "failed on attempt " << i;
}

TEST(RateLimiter, RejectsAboveMaxRate) {
  RateLimiter rl(3);
  rl.try_acquire();
  rl.try_acquire();
  rl.try_acquire();
  EXPECT_FALSE(rl.try_acquire());
}

TEST(RateLimiter, ResetsAfterOneSecond) {
  RateLimiter rl(2);
  EXPECT_TRUE(rl.try_acquire());
  EXPECT_TRUE(rl.try_acquire());
  EXPECT_FALSE(rl.try_acquire()); // full

  std::this_thread::sleep_for(std::chrono::milliseconds(1100));

  EXPECT_TRUE(rl.try_acquire()); // window has passed
}

TEST(RiskGateway, RateLimitRejectsExcessOrders) {
  MockGateway mock;
  RiskLimits lim{};
  lim.max_orders_per_sec = 2;
  RiskGateway gw(mock, lim);

  EXPECT_NE(gw.send_new(1, Side::Buy, 100, 1), kInvalidOrderId);
  EXPECT_NE(gw.send_new(1, Side::Buy, 100, 1), kInvalidOrderId);
  EXPECT_EQ(gw.send_new(1, Side::Buy, 100, 1), kInvalidOrderId);
  EXPECT_EQ(gw.rejected_rate(), 1u);
}

// ── fat-finger
// ────────────────────────────────────────────────────────────────

TEST(RiskGateway, FatFingerRejectsExtremePrice) {
  MockGateway mock;
  RiskLimits lim{};
  lim.fat_finger_bps = 100.0; // 1%
  RiskGateway gw(mock, lim);

  // ref mid = (9900 + 10100) / 2 = 10000
  gw.update_ref_price(1, 9900 * kPriceScale, 10100 * kPriceScale);

  // Order at 11000 → deviation = 10% = 1000 bps > 100 bps → reject
  EXPECT_EQ(gw.send_new(1, Side::Buy, 11000 * kPriceScale, 1), kInvalidOrderId);
  EXPECT_EQ(gw.rejected_fat_finger(), 1u);
}

TEST(RiskGateway, FatFingerAllowsNormalPrice) {
  MockGateway mock;
  RiskLimits lim{};
  lim.fat_finger_bps = 100.0; // 1%
  RiskGateway gw(mock, lim);

  gw.update_ref_price(1, 9990 * kPriceScale, 10010 * kPriceScale);

  // ref=10000, order=10050 → 0.5% = 50 bps < 100 bps → allow
  EXPECT_NE(gw.send_new(1, Side::Buy, 10050 * kPriceScale, 1), kInvalidOrderId);
  EXPECT_EQ(gw.rejected_fat_finger(), 0u);
}

TEST(RiskGateway, FatFingerAllowsIfNoRefPrice) {
  MockGateway mock;
  RiskLimits lim{};
  lim.fat_finger_bps = 100.0;
  RiskGateway gw(mock, lim);
  // No update_ref_price called → should allow (no reference to compare)
  EXPECT_NE(gw.send_new(1, Side::Buy, 99999 * kPriceScale, 1), kInvalidOrderId);
  EXPECT_EQ(gw.rejected_fat_finger(), 0u);
}

// ── position limits
// ───────────────────────────────────────────────────────────

TEST(RiskGateway, PositionLimitRejectsWhenExceeded) {
  MockGateway mock;
  RiskLimits lim{};
  lim.max_position_per_sym = 100;
  RiskGateway gw(mock, lim);

  // Buy 100 → at limit
  EXPECT_NE(gw.send_new(1, Side::Buy, 100, 100), kInvalidOrderId);
  // Buy 1 more → would be 101 → reject
  EXPECT_EQ(gw.send_new(1, Side::Buy, 100, 1), kInvalidOrderId);
  EXPECT_EQ(gw.rejected_position(), 1u);
}

TEST(RiskGateway, PositionUpdatedByNoteFill) {
  MockGateway mock;
  RiskLimits lim{};
  lim.max_position_per_sym = 100;
  RiskGateway gw(mock, lim);

  gw.send_new(1, Side::Buy, 100, 100); // tentative position = 100
  EXPECT_EQ(gw.position(1), 100);

  // Sell 50 → position = 50 → can buy 50 more
  gw.note_fill(1, Side::Sell, 50);
  EXPECT_EQ(gw.position(1), 50);
  EXPECT_NE(gw.send_new(1, Side::Buy, 100, 50), kInvalidOrderId);
}

// ── no limits (disabled) ─────────────────────────────────────────────────────

TEST(RiskGateway, AllLimitsDisabledPassesThrough) {
  MockGateway mock;
  RiskLimits lim{}; // all zeros = disabled
  RiskGateway gw(mock, lim);

  for (int i = 0; i < 5'000; ++i)
    EXPECT_NE(gw.send_new(1, Side::Buy, 1, 1), kInvalidOrderId);
}
