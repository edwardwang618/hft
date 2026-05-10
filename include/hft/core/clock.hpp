// include/hft/core/clock.hpp
#pragma once

#include <chrono>
#include <cstdint>

#if defined(__x86_64__) || defined(_M_X64)
#include <x86intrin.h> // __rdtsc, __rdtscp
#define HFT_ARCH_X86 1
#elif defined(__aarch64__)
#define HFT_ARCH_ARM64 1
#else
#error "Unsupported architecture"
#endif

#include "hft/core/types.hpp"

namespace hft::core {

// ─── Hardware cycle counter ────────────────────────────────────────────
// Reads the CPU's monotonic tick counter. Much cheaper than a syscall —
// ~6 ns on x86, ~3-10 ns on ARM64 depending on SoC.
//
// x86:     RDTSC    — may be reordered by the CPU.
// x86:     RDTSCP   — serializes load/stores before it (slightly slower).
// ARM64:   MRS cntvct_el0 — roughly analogous to RDTSC.
// ──────────────────────────────────────────────────────────────────────

[[gnu::always_inline]] inline uint64_t rdtsc() noexcept {
#if HFT_ARCH_X86
  return __rdtsc();
#else
  uint64_t v;
  asm volatile("mrs %0, cntvct_el0" : "=r"(v));
  return v;
#endif
}

[[gnu::always_inline]] inline uint64_t rdtscp() noexcept {
#if HFT_ARCH_X86
  unsigned aux;
  return __rdtscp(&aux);
#else
  // ARM64 has no direct equivalent. Add an ISB to prevent speculation
  // past the counter read — closest match semantically.
  uint64_t v;
  asm volatile("isb; mrs %0, cntvct_el0" : "=r"(v)::"memory");
  return v;
#endif
}

// ─── Wall / monotonic nanoseconds (syscall-backed) ─────────────────────

inline Ts wall_ns() noexcept {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

inline Ts mono_ns() noexcept {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// ─── TSC calibration ───────────────────────────────────────────────────
// Measure ticks_per_ns once at startup by sleeping ~10ms and comparing
// TSC delta to steady_clock delta.

struct TscCal {
  double ticks_per_ns;
  double ns_per_tick;
};

inline TscCal calibrate_tsc() noexcept {
#if HFT_ARCH_ARM64
  // On ARM64 we can just read cntfrq_el0 directly — no need to estimate.
  uint64_t freq_hz;
  asm volatile("mrs %0, cntfrq_el0" : "=r"(freq_hz));
  const double tpns = double(freq_hz) / 1e9;
  return {tpns, 1.0 / tpns};
#else
  using clk = std::chrono::steady_clock;
  const auto t0 = clk::now();
  const auto c0 = rdtsc();
  while (std::chrono::duration_cast<std::chrono::milliseconds>(clk::now() - t0)
             .count() < 10) { /* spin */
  }
  const auto c1 = rdtsc();
  const auto t1 = clk::now();

  const double ns =
      std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
  const double tpns = double(c1 - c0) / ns;
  return {tpns, 1.0 / tpns};
#endif
}

// Lazy singleton — calibration runs once on first use.
inline const TscCal &tsc_cal() noexcept {
  static const TscCal cal = calibrate_tsc();
  return cal;
}

inline Ts tsc_to_ns(uint64_t ticks) noexcept {
  return static_cast<Ts>(double(ticks) * tsc_cal().ns_per_tick);
}

} // namespace hft::core