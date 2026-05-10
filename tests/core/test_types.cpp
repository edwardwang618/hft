// tests/core/test_types.cpp
#include <cstdint>
#include <gtest/gtest.h>

#include "hft/core/types.hpp"

using namespace hft;

// ─── Price / Qty basics ────────────────────────────────────────────────

TEST(Types, PriceScaleIsTenThousand) { EXPECT_EQ(kPriceScale, 10'000); }

TEST(Types, PriceCanHoldTypicalRange) {
  // $0.01 min tick up to ~$1,000,000. Both fit comfortably in int64.
  const Price penny = 1;                             // $0.0001
  const Price million_usd = 1'000'000 * kPriceScale; // $1M
  EXPECT_LT(penny, million_usd);
  EXPECT_GT(million_usd, 0);
}

TEST(Types, InvalidPriceIsDistinguishable) {
  // INT64_MIN must not collide with any plausible real price.
  const Price real = 123 * kPriceScale;
  EXPECT_NE(kInvalidPrice, real);
  EXPECT_LT(kInvalidPrice, 0);
}

// ─── Side enum ─────────────────────────────────────────────────────────

TEST(Types, SideIsOneByte) {
  // enum class : uint8_t — critical for packing into messages.
  static_assert(sizeof(Side) == 1, "Side must be 1 byte");
}

TEST(Types, SideToString) {
  EXPECT_STREQ(to_string(Side::Buy), "BUY");
  EXPECT_STREQ(to_string(Side::Sell), "SELL");
}

TEST(Types, SideIsTypeSafe) {
  // This test is mostly about compile-time safety; we assert intent.
  // Side cannot implicitly convert to int — would fail compilation:
  //   int x = Side::Buy;   // error
  // So we just check the values.
  EXPECT_EQ(static_cast<uint8_t>(Side::Buy), 0);
  EXPECT_EQ(static_cast<uint8_t>(Side::Sell), 1);
}

// ─── Id sentinels ──────────────────────────────────────────────────────

TEST(Types, InvalidIdsAreZero) {
  EXPECT_EQ(kInvalidOrderId, 0u);
  EXPECT_EQ(kInvalidSymbolId, 0u);
}