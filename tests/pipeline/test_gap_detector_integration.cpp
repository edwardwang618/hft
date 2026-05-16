// Integration test: MockExchange + DroppingSink + FeedHandler + GapDetector.
//
// Pipeline:
//   MockExchange → DroppingSink → FeedHandler → GapDetector → RecordingSink
//
// Simulates UDP packet loss by having DroppingSink silently swallow
// specific sequence numbers. GapDetector detects the gap and calls
// on_gap_, which looks up the event from an in-memory retransmit store
// and re-injects it via the same on_md() path (synchronous 傻瓜重传).

#include <gtest/gtest.h>
#include <hft/exchange/binary_serializer.hpp>
#include <hft/exchange/byte_sink.hpp>
#include <hft/exchange/mock_exchange.hpp>
#include <hft/md/binary_parser.hpp>
#include <hft/md/wire_binary.hpp>
#include <hft/pipeline/feed_handler.hpp>
#include <hft/pipeline/gap_detector.hpp>

#include <cstring>
#include <map>
#include <set>

using namespace hft;
using namespace hft::pipeline;
using namespace hft::exchange;

namespace {

// Records (seq, event) pairs delivered by GapDetector.
struct RecordingSink {
  std::vector<std::pair<uint64_t, md::MdEvent>> received;
  void on_md(uint64_t seq, const md::MdEvent &ev) {
    received.emplace_back(seq, ev);
  }
};

// Sits between MockExchange and FeedHandler.
// Peeks at the binary wire::Header to read seq, drops the packet if listed.
struct DroppingSink : IByteSink {
  IByteSink        &downstream;
  std::set<uint64_t> drop_seqs;

  explicit DroppingSink(IByteSink &d) : downstream(d) {}

  void on_bytes(const std::byte *data, std::size_t n) noexcept override {
    if (n >= sizeof(md::wire::Header)) {
      md::wire::Header hdr{};
      std::memcpy(&hdr, data, sizeof(hdr));
      if (drop_seqs.count(hdr.seq)) return; // silently drop / 静默丢包
    }
    downstream.on_bytes(data, n);
  }
};

} // namespace

// ─────────────────────────────────────────────────────────────────────

TEST(GapDetectorIntegration, DropAndRecover) {
  using FH = FeedHandler<GapDetector<RecordingSink>, md::BinaryParser>;

  RecordingSink rec;

  // Retransmit store: seq → MdEvent.
  // In production this would be a TCP request to the exchange;
  // here we just look up the event we already know about.
  // 生产环境是向 exchange 发 TCP 重传请求；这里直接查内存 store。
  std::map<uint64_t, md::MdEvent> store;

  // on_gap_ needs to call det.on_md(), but det isn't constructed yet
  // when the lambda is created — use a pointer set right after construction.
  // lambda 创建时 det 尚未构造完成，用指针绕过先后顺序问题。
  GapDetector<RecordingSink> *det_ptr = nullptr;
  GapDetector<RecordingSink> det{
      rec,
      [&det_ptr, &store](uint64_t f, uint64_t t) {
        for (auto s = f; s <= t; ++s)
          det_ptr->on_md(s, store.at(s)); // 傻瓜重传 / naive retransmit
      }
  };
  det_ptr = &det;

  FH fh{det};
  DirectSink<FH>  direct{fh};
  DroppingSink    dropping{direct};

  BinarySerializer ser;
  constexpr uint64_t kStart = 1;
  MockExchange mx{dropping, ser, kStart};

  // Script 5 Add orders and build the retransmit store in parallel.
  std::vector<md::MdEvent> script = {
      md::Add{1, 1, 101, Side::Buy,  10000, 10},
      md::Add{2, 1, 102, Side::Sell, 10010,  5},
      md::Add{3, 1, 103, Side::Buy,   9990,  8},
      md::Add{4, 1, 104, Side::Sell, 10020,  3},
      md::Add{5, 1, 105, Side::Buy,   9980,  2},
  };
  for (std::size_t i = 0; i < script.size(); ++i) {
    store[kStart + i] = script[i];
    mx.push(script[i]);
  }

  // Simulate UDP loss: seq 2 and 3 never reach FeedHandler.
  dropping.drop_seqs = {2, 3};
  mx.run();

  // GapDetector should have triggered recovery for [2,3] and delivered
  // all 5 events in order to RecordingSink.
  // GapDetector 触发 [2,3] 的恢复请求，最终 5 条消息按序交付。
  ASSERT_EQ(rec.received.size(), 5u);
  for (uint64_t i = 0; i < 5; ++i)
    EXPECT_EQ(rec.received[i].first, kStart + i) << "out of order at i=" << i;
}

TEST(GapDetectorIntegration, NoDropNoGap) {
  using FH = FeedHandler<GapDetector<RecordingSink>, md::BinaryParser>;

  RecordingSink rec;
  std::map<uint64_t, md::MdEvent> store;
  GapDetector<RecordingSink> *det_ptr = nullptr;
  GapDetector<RecordingSink> det{
      rec,
      [&det_ptr, &store](uint64_t f, uint64_t t) {
        for (auto s = f; s <= t; ++s)
          det_ptr->on_md(s, store.at(s));
      }
  };
  det_ptr = &det;

  FH fh{det};
  DirectSink<FH> direct{fh};
  DroppingSink   dropping{direct}; // drop_seqs empty — no drops

  BinarySerializer ser;
  MockExchange mx{dropping, ser, /*start_seq=*/100};

  mx.push(md::Add{1, 7, 42, Side::Buy,  10000, 5});
  mx.push(md::Cancel{3, 7, 42});
  for (std::size_t i = 0; i < mx.script().size(); ++i)
    store[100 + i] = mx.script()[i];

  mx.run();

  EXPECT_EQ(rec.received.size(), 2u);
  EXPECT_EQ(rec.received[0].first, 100u);
  EXPECT_EQ(rec.received[1].first, 101u);
  EXPECT_FALSE(det.in_gap());
}
