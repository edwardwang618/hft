// include/hft/core/types.hpp
// Fundamental scalar types shared across the entire HFT framework.
// 整个 HFT 框架共享的基础标量类型。
#pragma once

#include <cstdint>

namespace hft {

// ─── Fixed-point price ────────────────────────────────────────────────
// 定点价格：64 位有符号整数，精度 4 位小数（kPriceScale = 10000）
using Price = int64_t;
inline constexpr int64_t kPriceScale = 10000; // 4 decimal places / 4 位小数

// ─── Quantity (signed to allow position deltas) ───────────────────────
// 数量（有符号，支持持仓变化量）
using Qty = int64_t;

// ─── Identifiers ──────────────────────────────────────────────────────
// 各类标识符
using SeqNum = uint64_t;
using OrderId = uint64_t;
using SymbolId = uint32_t;

// ─── Timestamp: nanoseconds since epoch ───────────────────────────────
// 时间戳：距纪元的纳秒数
using Ts = uint64_t;

// ─── Side ─────────────────────────────────────────────────────────────
// 买卖方向
enum class Side : uint8_t {
  Buy = 0,
  Sell = 1,
};

constexpr const char *to_string(Side s) {
  return s == Side::Buy ? "BUY" : "SELL";
}

// ─── Limit order ──────────────────────────────────────────────────────
// 限价单（POD，可安全拷贝）
struct Order {
  OrderId id;
  Side side;
  Price price;
  Qty qty;
  Ts ts;
};

// ─── Invalid sentinels ────────────────────────────────────────────────
// 无效值哨兵（用于错误检测）
inline constexpr Price kInvalidPrice = INT64_MIN;
inline constexpr Qty kInvalidQty = INT64_MIN;
inline constexpr OrderId kInvalidOrderId = 0;
inline constexpr SymbolId kInvalidSymbolId = 0;

} // namespace hft
