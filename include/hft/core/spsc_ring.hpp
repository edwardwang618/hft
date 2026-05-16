// include/hft/core/spsc_ring.hpp
// Lock-free, wait-free single-producer single-consumer ring buffer.
// 无锁、无等待的单生产者单消费者环形缓冲区。
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <new>
#include <type_traits>

namespace hft::core {

// C++17 gives us std::hardware_destructive_interference_size, but libstdc++
// guards it behind a feature test. 64 bytes is correct for x86_64 and all
// common ARM64 chips (Apple M-series, Graviton, Cortex-A7x, ...).
// C++17 提供了 std::hardware_destructive_interference_size，但 libstdc++
// 将其置于 特性检测宏之后。64 字节对 x86_64 及常见 ARM64 芯片（苹果 M
// 系列、Graviton 等）均正确。
#ifdef __cpp_lib_hardware_interference_size
// -Winterference-size is GCC-specific; clang doesn't know the group.
// -Winterference-size 是 GCC 专属警告组；Clang 不识别，需条件编译保护。
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Winterference-size"
#endif
inline constexpr std::size_t kCacheLine =
    std::hardware_destructive_interference_size;
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif
#else
inline constexpr std::size_t kCacheLine = 64;
#endif

// ─── Single-Producer Single-Consumer ring buffer ───────────────────────
// 单生产者单消费者环形缓冲区
//
// Lock-free, wait-free on the fast path. Fixed capacity, power-of-2.
// 快路径无锁无等待。固定容量，必须为 2 的幂次方。
// One slot is always unused (distinguishes empty vs full) → usable
// capacity is N - 1.
// 始终保留一个空槽以区分空与满，可用容量为 N - 1。
//
// Layout:
// 内存布局：
//   [ buffer ................................ ]   (shared, read-mostly /
//   共享，以读为主) [ head + cached_tail   (padded) ......... ]   (producer
//   cache line / 生产者缓存行) [ tail + cached_head   (padded) ......... ]
//   (consumer cache line / 消费者缓存行)
//
// Why cached indices? In steady state, producer only reads head_ (its own
// cache) and checks fullness against cached_tail_; only reloads the real
// tail_ when the cache says "full". Avoids a cache-line bounce on every push.
// 为何使用缓存索引？稳态下生产者仅读取自身 head_ 并用 cached_tail_ 判断是否满，
// 仅在缓存显示"满"时才重新加载真实的 tail_，避免每次 push 引发缓存行弹跳。
//
template <typename T, std::size_t N> class SpscRing {
  static_assert(N >= 2, "N must be >= 2");
  static_assert((N & (N - 1)) == 0, "N must be power of 2");
  static_assert(std::is_trivially_copyable_v<T>,
                "T should be trivially copyable for HFT paths");

public:
  SpscRing() = default;
  SpscRing(const SpscRing &) = delete;
  SpscRing &operator=(const SpscRing &) = delete;

  // Producer side. Returns false if ring is full.
  // 生产者接口。环满时返回 false。
  bool try_push(const T &item) noexcept {
    // relaxed: only the producer writes head_; no ordering needed here.
    // relaxed：仅生产者写 head_，此处无需排序。
    const auto head = head_.load(std::memory_order_relaxed);
    const auto next = (head + 1) & kMask;

    // Fast path: check cached tail first; reload real tail only if it says
    // full. 快路径：先检查缓存的 tail；仅在缓存显示满时才重新加载真实 tail。
    if (next == cached_tail_) {
      // acquire pairs with consumer's release-store when it advances tail_.
      // acquire 与消费者推进 tail_ 时的 release-store 配对。
      cached_tail_ = tail_.load(std::memory_order_acquire);
      if (next == cached_tail_)
        return false; // actually full / 确实已满
    }

    buffer_[head] = item;

    // release: commit payload write before publishing the new head.
    // Consumer's acquire-load of head_ will see this payload.
    // release：在发布新 head 之前提交 payload 写入。
    // 消费者对 head_ 的 acquire-load 可见此 payload。
    head_.store(next, std::memory_order_release);
    return true;
  }

  // Consumer side. Returns false if ring is empty.
  // 消费者接口。环空时返回 false。
  bool try_pop(T &out) noexcept {
    const auto tail = tail_.load(std::memory_order_relaxed);
    if (tail == cached_head_) {
      cached_head_ = head_.load(std::memory_order_acquire);
      if (tail == cached_head_)
        return false; // actually empty / 确实为空
    }
    out = buffer_[tail];
    tail_.store((tail + 1) & kMask, std::memory_order_release);
    return true;
  }

  // Best-effort size — exact only if no concurrent push/pop in progress.
  // 近似大小——仅在无并发 push/pop 时精确。
  std::size_t size_approx() const noexcept {
    const auto h = head_.load(std::memory_order_acquire);
    const auto t = tail_.load(std::memory_order_acquire);
    return (h - t) & kMask;
  }

  static constexpr std::size_t capacity() noexcept { return N - 1; }

private:
  static constexpr std::size_t kMask = N - 1;

  // Shared buffer. Aligned to its own cache line to avoid false sharing
  // with whatever precedes it in memory.
  // 共享缓冲区。对齐到独立缓存行，避免与前方内存发生伪共享。
  alignas(kCacheLine) std::array<T, N> buffer_{};

  // Producer-owned cache line.
  // 生产者独占缓存行。
  alignas(kCacheLine) std::atomic<std::size_t> head_{0};
  std::size_t cached_tail_{0};

  // Consumer-owned cache line.
  // 消费者独占缓存行。
  alignas(kCacheLine) std::atomic<std::size_t> tail_{0};
  std::size_t cached_head_{0};

  // Trailing pad so the next object can't false-share with tail_.
  // 尾部填充，防止紧接其后的对象与 tail_ 发生伪共享。
  char pad_tail_[kCacheLine]{};
};

} // namespace hft::core
