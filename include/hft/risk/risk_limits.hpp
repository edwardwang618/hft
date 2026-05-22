// include/hft/risk/risk_limits.hpp
// Pre-trade risk thresholds. All limits are enforced by RiskGateway.
// 预交易风险阈值。所有限制由 RiskGateway 强制执行。
#pragma once

#include <hft/core/types.hpp>

namespace hft::risk {

struct RiskLimits {
  // Maximum quantity in a single order. 0 = disabled.
  // 单笔最大数量。0 = 禁用。
  Qty max_order_qty{0};

  // Maximum notional (price × qty, in Price units) per order. 0 = disabled.
  // 单笔最大名义金额（价格 × 数量，Price 单位）。0 = 禁用。
  Price max_order_notional{0};

  // Maximum number of new orders allowed per second (sliding window). 0 =
  // disabled. 每秒最多允许的新订单数（滑动窗口）。0 = 禁用。
  int max_orders_per_sec{0};

  // Fat-finger: reject if |order_px - ref_px| / ref_px > threshold_bps / 10000.
  // ref_px must be set via RiskGateway::update_ref_price().
  // 胖手指：若 |委托价 - 参考价| / 参考价 > threshold_bps / 10000 则拒绝。
  // ref_px 须通过 RiskGateway::update_ref_price() 设置。
  double fat_finger_bps{0.0}; // 0 = disabled / 0 = 禁用

  // Maximum absolute net position per symbol (long or short). 0 = disabled.
  // 单品种最大净持仓（多或空）。0 = 禁用。
  Qty max_position_per_sym{0};
};

} // namespace hft::risk
