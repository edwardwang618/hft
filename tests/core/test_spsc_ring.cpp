#include "hft/core/spsc_ring.hpp"
#include <gtest/gtest.h>

#include <atomic>
#include <thread>
#include <vector>

using hft::core::SpscRing;

TEST(SpscRing, EmptyInitially) {
  SpscRing<int, 8> q;
  int x = 0;
  EXPECT_FALSE(q.try_pop(x));
  EXPECT_EQ(q.size_approx(), 0u);
  EXPECT_EQ(q.capacity(), 7u); // N - 1
}

TEST(SpscRing, SinglePushPop) {
  SpscRing<int, 8> q;
  ASSERT_TRUE(q.try_push(42));
  EXPECT_EQ(q.size_approx(), 1u);
  int x = 0;
  ASSERT_TRUE(q.try_pop(x));
  EXPECT_EQ(x, 42);
  EXPECT_EQ(q.size_approx(), 0u);
}

TEST(SpscRing, FillUpAndDrain) {
  SpscRing<int, 8> q; // capacity 7
  for (int i = 0; i < 7; ++i)
    ASSERT_TRUE(q.try_push(i));
  EXPECT_FALSE(q.try_push(999)); // full
  for (int i = 0; i < 7; ++i) {
    int x = -1;
    ASSERT_TRUE(q.try_pop(x));
    EXPECT_EQ(x, i);
  }
  int x;
  EXPECT_FALSE(q.try_pop(x));
}

TEST(SpscRing, WrapAround) {
  SpscRing<int, 4> q; // capacity 3
  // push-pop enough times to wrap the indices several times
  int expected = 0;
  for (int round = 0; round < 10; ++round) {
    ASSERT_TRUE(q.try_push(expected));
    ASSERT_TRUE(q.try_push(expected + 1));
    int x;
    ASSERT_TRUE(q.try_pop(x));
    EXPECT_EQ(x, expected);
    ASSERT_TRUE(q.try_pop(x));
    EXPECT_EQ(x, expected + 1);
    expected += 2;
  }
}

TEST(SpscRing, TwoThreadsFifoIntegrity) {
  constexpr int kN = 200'000;
  SpscRing<int, 1024> q;
  std::vector<int> out;
  out.reserve(kN);

  std::thread prod([&] {
    for (int i = 0; i < kN; ++i) {
      while (!q.try_push(i)) { /* spin */
      }
    }
  });

  std::thread cons([&] {
    int x;
    for (int i = 0; i < kN; ++i) {
      while (!q.try_pop(x)) { /* spin */
      }
      out.push_back(x);
    }
  });

  prod.join();
  cons.join();

  ASSERT_EQ(out.size(), static_cast<size_t>(kN));
  for (int i = 0; i < kN; ++i)
    EXPECT_EQ(out[i], i);
}