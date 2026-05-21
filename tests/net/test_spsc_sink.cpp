#include <hft/exchange/spsc_sink.hpp>

#include <gtest/gtest.h>

#include <cstring>
#include <thread>

using namespace hft;
using namespace hft::exchange;

static std::byte b(int v) { return static_cast<std::byte>(v & 0xFF); }

// ── single-threaded
// ───────────────────────────────────────────────────────────

TEST(SpscSink, PushPopRoundtrip) {
  SpscSink<16> sink;
  core::Packet<2048> out;

  const std::byte payload[4]{b(0xAA), b(0xBB), b(0xCC), b(0xDD)};
  sink.on_bytes(payload, 4);

  ASSERT_TRUE(sink.try_pop(out));
  EXPECT_EQ(out.len, 4u);
  EXPECT_EQ(std::memcmp(out.data.data(), payload, 4), 0);
  EXPECT_FALSE(sink.try_pop(out));
}

TEST(SpscSink, OversizedPacketDropped) {
  SpscSink<16, 8> sink; // MaxLen = 8
  core::Packet<8> out;

  std::byte big[9]{};
  sink.on_bytes(big, 9);

  EXPECT_FALSE(sink.try_pop(out));
  EXPECT_EQ(sink.dropped(), 1u);
}

TEST(SpscSink, RingFullDropsPackets) {
  SpscSink<4> sink; // usable capacity = N-1 = 3
  core::Packet<2048> out;

  std::byte one{b(1)};
  sink.on_bytes(&one, 1);
  sink.on_bytes(&one, 1);
  sink.on_bytes(&one, 1);
  sink.on_bytes(&one, 1); // 4th → ring full

  EXPECT_EQ(sink.dropped(), 1u);
  EXPECT_TRUE(sink.try_pop(out));
  EXPECT_TRUE(sink.try_pop(out));
  EXPECT_TRUE(sink.try_pop(out));
  EXPECT_FALSE(sink.try_pop(out));
}

TEST(SpscSink, EmptyRingReturnsFalse) {
  SpscSink<8> sink;
  core::Packet<2048> out;
  EXPECT_FALSE(sink.try_pop(out));
}

// ── two-thread
// ────────────────────────────────────────────────────────────────
//
// Ring is sized to hold all messages so the producer never stalls.
// Consumer verifies in-order delivery and zero drops.
// Ring 足够大，生产者不会阻塞；消费者验证有序交付且无丢包。

TEST(SpscSink, ProducerConsumerTwoThreads) {
  // N=1024, capacity=1023. kMsgs < capacity so producer never drops.
  // Heap-allocated: Packet<2048> * 1024 ≈ 2 MB — too large for the stack.
  // N=1024，容量=1023。kMsgs < 容量，生产者不会丢包。堆分配避免栈溢出。
  static constexpr int kMsgs = 800;
  auto sink = std::make_unique<SpscSink<1024>>();

  std::thread producer([&] {
    for (uint32_t i = 0; i < static_cast<uint32_t>(kMsgs); ++i)
      sink->on_bytes(reinterpret_cast<const std::byte *>(&i), sizeof(i));
  });

  core::Packet<2048> pkt;
  int received = 0;
  while (received < kMsgs) {
    if (sink->try_pop(pkt)) {
      ASSERT_EQ(pkt.len, sizeof(uint32_t));
      uint32_t val;
      std::memcpy(&val, pkt.data.data(), sizeof(val));
      EXPECT_EQ(val, static_cast<uint32_t>(received));
      ++received;
    }
  }

  producer.join();
  EXPECT_EQ(sink->dropped(), 0u);
}