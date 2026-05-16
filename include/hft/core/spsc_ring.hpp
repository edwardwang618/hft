// include/hft/core/spsc_ring.hpp
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <new>
#include <type_traits>

namespace hft::core {

// C++17 gives us std::hardware_destructive_interference_size, but libstdc++
// guards it behind a feature test. 64 is correct for all x86_64 and every
// ARM64 chip I know of (Apple M-series, Graviton, Cortex-A7x, ...).
#ifdef __cpp_lib_hardware_interference_size
// -Winterference-size fires on any use of the stdlib name, even to initialize
// a user-defined constant. Suppress it here; all callers use kCacheLine.
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Winterference-size"
inline constexpr std::size_t kCacheLine =
    std::hardware_destructive_interference_size;
#  pragma GCC diagnostic pop
#else
inline constexpr std::size_t kCacheLine = 64;
#endif

// ─── Single-Producer Single-Consumer ring buffer ───────────────────────
// Lock-free, wait-free on the fast path. Fixed capacity, power-of-2.
// One slot is always unused (distinguishes empty vs full) → usable
// capacity is N - 1.
//
// Layout:
//   [ buffer .............................. ]   (shared, read-mostly)
//   [ head + cached_tail    (padded) ......  ]  (producer's cache line)
//   [ tail + cached_head    (padded) ......  ]  (consumer's cache line)
//
// Why cached indices? In steady state, producer only needs head_ (which
// lives in its own cache). It checks fullness against cached_tail_
// — only reloads the real tail_ when the cache says "full". This avoids
// a cacheline bounce on every push.
// ──────────────────────────────────────────────────────────────────────
template <typename T, std::size_t N> class SpscRing {
  static_assert(N >= 2, "N must be >= 2");
  static_assert((N & (N - 1)) == 0, "N must be power of 2");
  static_assert(std::is_trivially_copyable_v<T>,
                "T should be trivially copyable for HFT paths");

public:
  SpscRing() = default;
  SpscRing(const SpscRing &) = delete;
  SpscRing &operator=(const SpscRing &) = delete;

  // Producer side. Returns false if full.
  bool try_push(const T &item) noexcept {
    // relaxed: only the producer writes head_, so we always see our own
    // latest value. No ordering needed against other threads here.
    const auto head = head_.load(std::memory_order_relaxed);
    const auto next = (head + 1) & kMask;

    // Fast path: check against our cached view of tail.
    if (next == cached_tail_) {
      // Cache says full — reload real tail. acquire pairs with the
      // consumer's release-store when it advances tail.
      cached_tail_ = tail_.load(std::memory_order_acquire);
      if (next == cached_tail_)
        return false; // actually full
    }

    buffer_[head] = item;

    // release: commit the payload write *before* publishing the new head.
    // Consumer's acquire-load of head_ will see this payload.
    head_.store(next, std::memory_order_release);
    return true;
  }

  // Consumer side. Returns false if empty.
  bool try_pop(T &out) noexcept {
    const auto tail = tail_.load(std::memory_order_relaxed);

    if (tail == cached_head_) {
      cached_head_ = head_.load(std::memory_order_acquire);
      if (tail == cached_head_)
        return false; // actually empty
    }

    out = buffer_[tail];

    tail_.store((tail + 1) & kMask, std::memory_order_release);
    return true;
  }

  // Best-effort size — exact only if no one's pushing/popping concurrently.
  std::size_t size_approx() const noexcept {
    const auto h = head_.load(std::memory_order_acquire);
    const auto t = tail_.load(std::memory_order_acquire);
    return (h - t) & kMask;
  }

  static constexpr std::size_t capacity() noexcept { return N - 1; }

private:
  static constexpr std::size_t kMask = N - 1;

  // Shared buffer. Aligned to its own line so it doesn't share with whatever
  // comes before it in memory.
  alignas(kCacheLine) std::array<T, N> buffer_{};

  // Producer-owned cache line.
  alignas(kCacheLine) std::atomic<std::size_t> head_{0};
  std::size_t cached_tail_{0};

  // Consumer-owned cache line.
  alignas(kCacheLine) std::atomic<std::size_t> tail_{0};
  std::size_t cached_head_{0};

  // Trailing pad so the next object in memory can't false-share with tail_.
  char pad_tail_[kCacheLine]{};
};

} // namespace hft::core