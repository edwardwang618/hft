// include/hft/risk/risk_gateway.hpp
// Pre-trade risk wrapper around IOrderGateway.
// 围绕 IOrderGateway 的预交易风险检查包装器。
//
// Sits between Strategy and the real gateway (OrderManager / OrderGateway):
//   Strategy → RiskGateway → IOrderGateway (real wire)
//
// On any check failure: increments the relevant counter, returns
// kInvalidOrderId. On kill switch: all send_new / send_cancel / send_replace
// are no-ops.
//
// 位于策略与真实网关之间。检查失败时：递增对应计数器，返回 kInvalidOrderId。
// 触发 kill switch 后：所有发单/撤单/改单均为空操作。
//
// Thread safety: kill() / unkill() are atomic. Everything else is NOT
// thread-safe — call from the single hot-path thread only.
// 线程安全：kill()/unkill() 是原子的；其余接口仅限热路径单线程调用。
#pragma once

#include <hft/core/types.hpp>
#include <hft/md/md_event.hpp>
#include <hft/risk/rate_limiter.hpp>
#include <hft/risk/risk_limits.hpp>
#include <hft/strategy/i_order_gateway.hpp>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <unordered_map>

namespace hft::risk {

class RiskGateway final : public strategy::IOrderGateway {
public:
  RiskGateway(strategy::IOrderGateway &next, const RiskLimits &limits)
      : next_(next), limits_(limits), rate_limiter_(limits.max_orders_per_sec) {
  }

  // ── Kill switch ───────────────────────────────────────────────────
  // Thread-safe: may be called from any thread (e.g. a monitoring thread).
  // 线程安全：可从任意线程调用（如监控线程）。
  void kill() noexcept { killed_.store(true, std::memory_order_release); }
  void unkill() noexcept { killed_.store(false, std::memory_order_release); }
  bool is_killed() const noexcept {
    return killed_.load(std::memory_order_acquire);
  }

  // Update reference price for fat-finger checks (call on each on_bbo).
  // mid = (bid + ask) / 2; if bid or ask is 0 (empty side), skip update.
  // 更新胖手指检查的参考价（每次 on_bbo 时调用）。
  // mid = (bid + ask) / 2；若买/卖任一为空则跳过更新。
  void update_ref_price(md::SymbolId sym, Price bid, Price ask) noexcept {
    if (bid > 0 && ask > 0)
      ref_prices_[sym] = (bid + ask) / 2;
  }

  // Notify RiskGateway of a fill so it can update its position counters.
  // Call this from on_fill before forwarding to the strategy.
  // 通知 RiskGateway 成交，以更新持仓计数器。在 on_fill 转发前调用。
  void note_fill(md::SymbolId sym, Side side, Qty filled_qty) noexcept {
    const int64_t delta = (side == Side::Buy)
                              ? static_cast<int64_t>(filled_qty)
                              : -static_cast<int64_t>(filled_qty);
    positions_[sym] += delta;
  }

  // ── IOrderGateway ─────────────────────────────────────────────────

  OrderId send_new(md::SymbolId sym, Side side, Price px, Qty qty) override {
    if (is_killed()) {
      ++rejected_kill_;
      return kInvalidOrderId;
    }

    if (!check_qty(qty))
      return kInvalidOrderId;
    if (!check_notional(px, qty))
      return kInvalidOrderId;
    if (!check_rate())
      return kInvalidOrderId;
    if (!check_fat_finger(sym, px))
      return kInvalidOrderId;
    if (!check_position(sym, side, qty))
      return kInvalidOrderId;

    return next_.send_new(sym, side, px, qty);
  }

  void send_cancel(OrderId id) override {
    if (is_killed()) {
      ++rejected_kill_;
      return;
    }
    next_.send_cancel(id);
  }

  void send_replace(OrderId old_id, Price new_px, Qty new_qty) override {
    if (is_killed()) {
      ++rejected_kill_;
      return;
    }
    if (!check_qty(new_qty))
      return;
    if (!check_notional(new_px, new_qty))
      return;
    next_.send_replace(old_id, new_px, new_qty);
  }

  // ── Diagnostics ───────────────────────────────────────────────────
  uint64_t rejected_kill() const noexcept { return rejected_kill_; }
  uint64_t rejected_qty() const noexcept { return rejected_qty_; }
  uint64_t rejected_notional() const noexcept { return rejected_notional_; }
  uint64_t rejected_rate() const noexcept { return rejected_rate_; }
  uint64_t rejected_fat_finger() const noexcept { return rejected_fat_finger_; }
  uint64_t rejected_position() const noexcept { return rejected_position_; }

  int64_t position(md::SymbolId sym) const noexcept {
    auto it = positions_.find(sym);
    return it == positions_.end() ? 0 : it->second;
  }

private:
  strategy::IOrderGateway &next_;
  RiskLimits limits_;
  RateLimiter rate_limiter_;
  std::atomic<bool> killed_{false};

  std::unordered_map<md::SymbolId, Price> ref_prices_;
  std::unordered_map<md::SymbolId, int64_t> positions_;

  uint64_t rejected_kill_{0};
  uint64_t rejected_qty_{0};
  uint64_t rejected_notional_{0};
  uint64_t rejected_rate_{0};
  uint64_t rejected_fat_finger_{0};
  uint64_t rejected_position_{0};

  bool check_qty(Qty qty) noexcept {
    if (limits_.max_order_qty > 0 && qty > limits_.max_order_qty) {
      ++rejected_qty_;
      return false;
    }
    return true;
  }

  bool check_notional(Price px, Qty qty) noexcept {
    if (limits_.max_order_notional > 0 && px > 0) {
      // Use __int128 to avoid overflow on large px*qty.
      // 用 __int128 避免大 px*qty 溢出。
      const __int128 notional = static_cast<__int128>(px) * qty;
      if (notional > static_cast<__int128>(limits_.max_order_notional)) {
        ++rejected_notional_;
        return false;
      }
    }
    return true;
  }

  bool check_rate() noexcept {
    if (limits_.max_orders_per_sec <= 0)
      return true; // 0 = disabled
    if (!rate_limiter_.try_acquire()) {
      ++rejected_rate_;
      return false;
    }
    return true;
  }

  bool check_fat_finger(md::SymbolId sym, Price px) noexcept {
    if (limits_.fat_finger_bps <= 0.0 || px <= 0)
      return true;
    auto it = ref_prices_.find(sym);
    if (it == ref_prices_.end())
      return true; // no ref yet, allow
    const Price ref = it->second;
    if (ref <= 0)
      return true;
    const double deviation_bps = std::abs(static_cast<double>(px - ref)) /
                                 static_cast<double>(ref) * 10000.0;
    if (deviation_bps > limits_.fat_finger_bps) {
      ++rejected_fat_finger_;
      return false;
    }
    return true;
  }

  bool check_position(md::SymbolId sym, Side side, Qty qty) noexcept {
    if (limits_.max_position_per_sym <= 0)
      return true;
    auto it = positions_.find(sym);
    const int64_t current = (it != positions_.end()) ? it->second : 0;
    const int64_t delta = (side == Side::Buy) ? static_cast<int64_t>(qty)
                                              : -static_cast<int64_t>(qty);
    const int64_t after = current + delta;
    if (std::abs(after) > static_cast<int64_t>(limits_.max_position_per_sym)) {
      ++rejected_position_;
      return false;
    }
    // Tentatively update — will be corrected by note_fill on actual execution.
    // 乐观更新——实际成交后由 note_fill 修正。
    positions_[sym] = after;
    return true;
  }
};

} // namespace hft::risk
