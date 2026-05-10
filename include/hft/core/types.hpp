// include/hft/core/types.hpp
#pragma once

#include <cstdint>

namespace hft {

// ─── Fixed-point price ────────────────────────────────────────────────
using Price = int64_t;
inline constexpr int64_t kPriceScale = 10000; // 4 decimals

// ─── Quantity (signed to allow position deltas) ───────────────────────
using Qty = int64_t;

// ─── Identifiers ──────────────────────────────────────────────────────
using SeqNum = uint64_t;
using OrderId = uint64_t;
using SymbolId = uint32_t;

// ─── Timestamp: nanoseconds ───────────────────────────────────────────
using Ts = uint64_t;

// ─── Side ─────────────────────────────────────────────────────────────
enum class Side : uint8_t {
  Buy = 0,
  Sell = 1,
};

constexpr const char *to_string(Side s) {
  return s == Side::Buy ? "BUY" : "SELL";
}

// ─── Invalid sentinels ────────────────────────────────────────────────
inline constexpr Price kInvalidPrice = INT64_MIN;
inline constexpr Qty kInvalidQty = INT64_MIN;
inline constexpr OrderId kInvalidOrderId = 0;
inline constexpr SymbolId kInvalidSymbolId = 0;

} // namespace hft