// tests/core/test_clock.cpp
#include <cstdint>
#include <cstdlib>
#include <gtest/gtest.h>

#include <chrono>
#include <iostream>
#include <thread>

#include "hft/core/clock.hpp"

using namespace hft;
using namespace hft::core;

// ─── Wall clock sanity ─────────────────────────────────────────────────

TEST(Clock, WallNsIsNonZero) { EXPECT_GT(wall_ns(), 0u); }

TEST(Clock, WallNsIsRoughlyNow) {
  // Should be within a reasonable range of "now" as seen by std::chrono.
  // We allow 1 second slack — plenty for any machine.
  const auto ours = wall_ns();
  const auto ref = std::chrono::duration_cast<std::chrono::nanoseconds>(
                       std::chrono::system_clock::now().time_since_epoch())
                       .count();

  const int64_t diff = static_cast<int64_t>(ref) - static_cast<int64_t>(ours);
  EXPECT_LT(std::abs(diff), 1'000'000'000); // < 1 s
}

TEST(Clock, WallNsAdvances) {
  const auto t0 = wall_ns();
  std::this_thread::sleep_for(std::chrono::milliseconds(1));
  const auto t1 = wall_ns();
  EXPECT_GT(t1, t0);
}

TEST(Clock, MonoNsIsMonotonic) {
  // Sample many times in a tight loop; must never go backwards.
  Ts prev = mono_ns();
  for (int i = 0; i < 10'000; ++i) {
    const Ts now = mono_ns();
    ASSERT_GE(now, prev);
    prev = now;
  }
}

// ─── TSC sanity ────────────────────────────────────────────────────────

TEST(Clock, RdtscAdvances) {
  const auto c0 = rdtsc();
  // A few hundred cycles of work.
  volatile int sink = 0;
  for (int i = 0; i < 1000; ++i)
    sink += i;
  const auto c1 = rdtsc();
  EXPECT_GT(c1, c0);
}

TEST(Clock, RdtscpAdvances) {
  const auto c0 = rdtscp();
  volatile int sink = 0;
  for (int i = 0; i < 1000; ++i)
    sink += i;
  const auto c1 = rdtscp();
  EXPECT_GT(c1, c0);
}

// ─── Calibration ───────────────────────────────────────────────────────

TEST(Clock, CalibrationReturnsPlausibleFrequency) {
  const auto &cal = tsc_cal();
  // Modern x86 boxes: TSC runs at 1-5 GHz base freq, i.e. 1-5 ticks / ns.
  EXPECT_GT(cal.ticks_per_ns, 0.01);
  EXPECT_LT(cal.ticks_per_ns, 10.0);
  // Reciprocal should match.
  EXPECT_NEAR(cal.ns_per_tick * cal.ticks_per_ns, 1.0, 1e-9);
}

TEST(Clock, TscToNsRoughlyMatchesWallClock) {
  // Sleep 10 ms, compare TSC-derived duration to wall clock.
  // Allow ±20% slack — this test runs on shared CI machines.
  const auto w0 = mono_ns();
  const auto c0 = rdtsc();

  std::this_thread::sleep_for(std::chrono::milliseconds(10));

  const auto c1 = rdtsc();
  const auto w1 = mono_ns();

  const Ts tsc_ns = tsc_to_ns(c1 - c0);
  const Ts wall_elapsed = w1 - w0;

  const double ratio = double(tsc_ns) / double(wall_elapsed);
  EXPECT_GT(ratio, 0.80);
  EXPECT_LT(ratio, 1.20);
}

// ─── Overhead measurement (not a correctness test, but informative) ────

TEST(Clock, RdtscIsFasterThanWallNs) {
  constexpr int N = 100'000;

  const auto s1 = rdtsc();
  for (int i = 0; i < N; ++i) {
    volatile auto x = rdtsc();
    (void)x;
  }
  const auto e1 = rdtsc();
  const Ts rdtsc_total = tsc_to_ns(e1 - s1);

  const auto s2 = rdtsc();
  for (int i = 0; i < N; ++i) {
    volatile auto x = wall_ns();
    (void)x;
  }
  const auto e2 = rdtsc();
  const Ts wall_total = tsc_to_ns(e2 - s2);

  // rdtsc should be meaningfully faster. On most boxes wall_ns is 2-5x slower.
  EXPECT_LT(rdtsc_total, wall_total);

  // Print for humans — visible with `ctest -V` or gtest --gtest_print_time.
  std::cerr << "rdtsc avg: " << (rdtsc_total / N) << " ns, "
            << "wall_ns avg: " << (wall_total / N) << " ns\n";
}