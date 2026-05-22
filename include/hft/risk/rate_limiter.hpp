// include/hft/risk/rate_limiter.hpp
// Sliding-window rate limiter: accurate O(1) orders-per-second enforcement.
// 滑动窗口限速器：精确 O(1) 的每秒订单数限制。
//
// Keeps a circular buffer of the last `max_rate` order timestamps.
// try_acquire() returns true if the oldest slot in the buffer is older than
// one second — i.e., we have not sent `max_rate` orders in the last second.
//
// 保存最近 max_rate 笔订单的时间戳环形缓冲区。
// 若缓冲区最旧槽距现在超过 1 秒，则允许通过（说明最近 1 秒内不足 max_rate
// 笔）。
#pragma once

#include <hft/core/clock.hpp>

#include <cstdint>
#include <vector>

namespace hft::risk {

class RateLimiter {
public:
  explicit RateLimiter(int max_orders_per_sec)
      : max_rate_(max_orders_per_sec),
        ts_(static_cast<std::size_t>(max_orders_per_sec), 0) {}

  // Returns true and records the timestamp if the order is within rate.
  // Returns false (and does NOT record) if the rate limit would be exceeded.
  // 在限速范围内时返回 true 并记录时间戳；超限时返回 false，不记录。
  bool try_acquire() noexcept {
    if (max_rate_ <= 0)
      return false;

    const uint64_t now = core::mono_ns();
    const std::size_t oldest_slot =
        static_cast<std::size_t>(head_) % ts_.size();

    // If we haven't filled the buffer yet, or the oldest order was > 1s ago.
    // 缓冲区未满，或最旧订单超过 1 秒前。
    if (count_ < max_rate_ || now - ts_[oldest_slot] >= kOneSecNs) {
      ts_[oldest_slot] = now;
      head_ = static_cast<int>((oldest_slot + 1) % ts_.size());
      if (count_ < max_rate_)
        ++count_;
      return true;
    }
    return false;
  }

  int max_rate() const noexcept { return max_rate_; }

private:
  static constexpr uint64_t kOneSecNs = 1'000'000'000ULL;

  int max_rate_;
  std::vector<uint64_t> ts_;
  int head_{0};
  int count_{0};
};

} // namespace hft::risk
