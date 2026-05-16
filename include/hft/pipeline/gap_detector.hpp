// include/hft/pipeline/gap_detector.hpp
// UDP sequence-number gap detector and reorder buffer.
// UDP 序列号 gap 检测器与乱序缓冲区。
#pragma once

#include <hft/md/md_event.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>

namespace hft::pipeline {

// ─── GapDetector ──────────────────────────────────────────────────────
//
// Pipeline stage between FeedHandler and BookBuilder.
// Detects missing sequence numbers (UDP packet loss / reordering),
// buffers out-of-order messages, and triggers recovery.
//
// 流水线阶段，位于 FeedHandler 和 BookBuilder 之间。
// 检测序列号空洞（UDP 丢包/乱序），缓冲乱序消息，并触发恢复请求。
//
// Pipeline / 流水线：
//   FeedHandler<GapDetector<BookBuilder>, Parser>
//
// Three cases per message / 每条消息三种情况：
//   seq <  expected  → duplicate, drop         重复包，丢弃
//   seq == expected  → in-order, deliver+drain  正常，交付并尝试排干缓冲
//   seq >  expected  → gap/reorder, buffer      空洞/乱序，入缓冲并请求恢复
//
// Recovery / 恢复：
//   The on_gap callback signals the application to fetch missing messages
//   (retransmit request, TCP refresh channel, etc.).
//   on_gap 回调通知应用层获取缺失消息（重传请求、TCP 补包通道等）。
//   Recovery messages re-enter via the same on_md() on the same thread.
//   恢复消息通过同一线程的同一 on_md() 接口重新送入。
//
// Overflow / 溢出：
//   If the buffer exceeds max_buffer, recovery is too slow.
//   The on_overflow callback should initiate a full book snapshot,
//   then call reset(next_seq) to re-anchor the expected sequence.
//   若缓冲超过 max_buffer，说明恢复过慢，应通过 on_overflow 请求全量快照，
//   然后调用 reset(next_seq) 重新锚定期望序列号。
//
template <class Next> class GapDetector {
public:
  // on_gap(from, to): request retransmit of [from, to] inclusive.
  // on_gap(from, to)：请求重传 [from, to] 区间（含两端）的消息。
  using RecoveryFn = std::function<void(uint64_t from, uint64_t to)>;

  // on_overflow(): buffer too large, request a full snapshot.
  // on_overflow()：缓冲区过大，请求全量快照。
  using SnapshotFn = std::function<void()>;

  explicit GapDetector(Next &next, RecoveryFn on_gap,
                       SnapshotFn on_overflow = {},
                       std::size_t max_buffer = 1024)
      : next_(next), on_gap_(std::move(on_gap)),
        on_overflow_(std::move(on_overflow)), max_buffer_(max_buffer) {}

  // ── Main entry point (called by FeedHandler) ──────────────────────
  // 主入口（由 FeedHandler 调用）

  void on_md(uint64_t seq, const md::MdEvent &e) {
    if (!initialized_) {
      expected_ = seq;
      recovery_req_through_ =
          seq - 1; // nothing requested yet / 尚未请求任何恢复
      initialized_ = true;
    }

    if (seq < expected_) {
      // Duplicate or stale retransmit. / 重复包或过期重传包。
      ++dropped_;
      return;
    }

    if (seq == expected_) {
      next_.on_md(seq, e);
      ++expected_;
      drain();
      return;
    }

    // seq > expected_: gap or out-of-order. / 空洞或乱序。
    buffer_.emplace(seq, e); // emplace is a no-op if seq already buffered
                             // 若 seq 已在缓冲中，emplace 不覆盖
    request_(expected_, seq - 1);

    if (buffer_.size() > max_buffer_ && on_overflow_)
      on_overflow_();
  }

  // ── Called after receiving a full snapshot ────────────────────────
  // 接收全量快照后调用
  //
  // next_seq: first live sequence number to expect after the snapshot.
  // next_seq：快照之后期望收到的第一个实时序列号。
  void reset(uint64_t next_seq) {
    buffer_.clear();
    expected_ = next_seq;
    recovery_req_through_ = next_seq - 1;
    in_gap_ = false;
    initialized_ = true;
  }

  // ── Diagnostics ───────────────────────────────────────────────────
  // 诊断接口

  bool in_gap() const noexcept { return in_gap_; }
  uint64_t expected_seq() const noexcept { return expected_; }
  std::size_t buffered() const noexcept { return buffer_.size(); }
  uint64_t dropped() const noexcept { return dropped_; }

private:
  Next &next_;
  RecoveryFn on_gap_;
  SnapshotFn on_overflow_;
  std::size_t max_buffer_;

  uint64_t expected_{1};
  uint64_t recovery_req_through_{
      0}; // highest seq already requested / 已请求恢复的最高 seq
  bool initialized_{false};
  bool in_gap_{false};
  uint64_t dropped_{0};

  // Reorder buffer sorted by seq. / 按 seq 排序的乱序缓冲区。
  std::map<uint64_t, md::MdEvent> buffer_;

  // Request recovery for [from, to], skipping already-requested range.
  // 仅针对尚未请求过的范围调用 on_gap_。
  void request_(uint64_t from, uint64_t to) {
    if (to < from)
      return;
    const uint64_t req_from = std::max(from, recovery_req_through_ + 1);
    if (req_from > to)
      return; // already covered / 已覆盖，无需重复请求
    in_gap_ = true;
    on_gap_(req_from, to);
    recovery_req_through_ = to;
  }

  // Deliver buffered messages that are now in-order.
  // Detect any new gaps and request recovery for them.
  // 交付缓冲区中已按序的消息；检测新空洞并请求恢复。
  void drain() {
    while (!buffer_.empty()) {
      auto it = buffer_.begin();

      if (it->first < expected_) {
        // Stale entry (e.g. recovery duplicate). /
        // 过期条目（如重传产生的重复）。
        buffer_.erase(it);
        continue;
      }

      if (it->first > expected_) {
        // Gap between expected_ and next buffered seq.
        // 缓冲区前端存在空洞。
        request_(expected_, it->first - 1);
        return;
      }

      // it->first == expected_: deliver.
      next_.on_md(it->first, it->second);
      ++expected_;
      buffer_.erase(it);
    }

    // Buffer fully drained with no gaps remaining.
    // 缓冲区已完全排干，无剩余空洞。
    in_gap_ = false;
  }
};

} // namespace hft::pipeline
