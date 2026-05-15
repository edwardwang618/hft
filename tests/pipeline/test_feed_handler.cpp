// tests/pipeline/test_feed_handler.cpp
#include "hft/core/types.hpp"
#include "wire_fixtures.hpp"
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <gtest/gtest.h>
#include <hft/md/wire_binary.hpp>
#include <hft/pipeline/feed_handler.hpp>
#include <span>
#include <variant>
#include <vector>

using namespace hft;
using namespace hft::md;
using namespace hft::test;

namespace {

struct CollectingSink {
  std::vector<MdEvent> events;
  void on_md(std::uint64_t /*seq*/, const MdEvent &e) { events.push_back(e); }
};

struct SeqRecordingSink {
  std::vector<std::uint64_t> seqs;
  void on_md(std::uint64_t seq, const MdEvent &) noexcept {
    seqs.push_back(seq);
  }
};

} // namespace

// ================================================================
// 1. 一次喂完整 1 条 -- fast path
// ================================================================
TEST(FeedHandler, SingleCompleteMessage) {
  CollectingSink sink;
  pipeline::FeedHandler<CollectingSink> fh(sink);

  auto b = make_add(/*seq=*/0, /*ts=*/1000, /*sym=*/42, /*id=*/7, Side::Buy,
                    /*px=*/12345, /*qty=*/10);

  fh.on_bytes(b.data(), b.size());

  ASSERT_EQ(sink.events.size(), 1u);
  ASSERT_TRUE(std::holds_alternative<Add>(sink.events[0]));
  const auto &a = std::get<Add>(sink.events[0]);
  EXPECT_EQ(a.ts, 1000);
  EXPECT_EQ(a.sym, 42u);
  EXPECT_EQ(a.id, 7u);
  EXPECT_EQ(a.side, Side::Buy);
  EXPECT_EQ(a.px, 12345);
  EXPECT_EQ(a.qty, 10);

  EXPECT_EQ(fh.msgs_parsed(), 1u);
  EXPECT_EQ(fh.bytes_consumed(), b.size());
  EXPECT_EQ(fh.buffered(), 0u);
}

// ================================================================
// 2. 一条 message 切成两段喂
// ================================================================
TEST(FeedHandler, SplitAcrossTwoChunks) {
  CollectingSink sink;
  pipeline::FeedHandler<CollectingSink> fh(sink);

  auto b = make_add(/*seq=*/1, /*ts=*/1000, /*sym=*/42, /*id=*/7, Side::Buy,
                    /*px=*/12345, /*qty=*/10);
  ASSERT_GT(b.size(), 1u);

  // 第一段: 1 byte (连 header 都不完整)
  fh.on_bytes(b.data(), 1);
  EXPECT_EQ(sink.events.size(), 0u);
  EXPECT_EQ(fh.msgs_parsed(), 0u);
  EXPECT_EQ(fh.buffered(), 1u);

  // 第二段: 剩下全部
  fh.on_bytes(b.data() + 1, b.size() - 1);
  ASSERT_EQ(sink.events.size(), 1u);
  EXPECT_TRUE(std::holds_alternative<Add>(sink.events[0]));
  EXPECT_EQ(fh.msgs_parsed(), 1u);
  EXPECT_EQ(fh.bytes_consumed(), b.size());
  EXPECT_EQ(fh.buffered(), 0u);
}

// ================================================================
// 3. 一次喂 3 条 -- loop 驱动
// ================================================================
TEST(FeedHandler, ThreeMessagesInOneChunk) {
  CollectingSink sink;
  pipeline::FeedHandler<CollectingSink> fh(sink);

  auto all = concat({
      make_add(/*seq=*/1, /*ts=*/42, /*sym=*/1, /*id=*/100, Side::Buy,
               /*px=*/100, /*qty=*/5),
      make_cancel(/*seq=*/2, /*ts=*/42, /*sym=*/1, /*id=*/100),
      make_add(/*seq=*/3, /*ts=*/42, /*sym=*/1, /*id=*/101, Side::Sell,
               /*px=*/200, /*qty=*/10),
  });

  fh.on_bytes(all.data(), all.size());

  ASSERT_EQ(sink.events.size(), 3u);
  EXPECT_TRUE(std::holds_alternative<Add>(sink.events[0]));
  EXPECT_TRUE(std::holds_alternative<Cancel>(sink.events[1]));
  EXPECT_TRUE(std::holds_alternative<Add>(sink.events[2]));

  EXPECT_EQ(std::get<Add>(sink.events[0]).id, 100u);
  EXPECT_EQ(std::get<Cancel>(sink.events[1]).id, 100u);
  EXPECT_EQ(std::get<Add>(sink.events[2]).id, 101u);
  EXPECT_EQ(std::get<Add>(sink.events[2]).side, Side::Sell);

  EXPECT_EQ(fh.msgs_parsed(), 3u);
  EXPECT_EQ(fh.bytes_consumed(), all.size());
  EXPECT_EQ(fh.buffered(), 0u);
}

// ================================================================
// 4. 1 条完整 + 下一条头几字节
// ================================================================
TEST(FeedHandler, OneFullPlusPartial) {
  CollectingSink sink;
  pipeline::FeedHandler<CollectingSink> fh(sink);

  auto m1 = make_add(/*seq=*/1, /*ts=*/42, /*sym=*/1, /*id=*/100, Side::Buy,
                     /*px=*/100, /*qty=*/5);
  auto m2 = make_add(/*seq=*/2, /*ts=*/42, /*sym=*/1, /*id=*/101, Side::Buy,
                     /*px=*/200, /*qty=*/10);
  ASSERT_GT(m2.size(), 3u);

  // m1 完整 + m2 前 3 字节
  std::vector<std::byte> first;
  first.insert(first.end(), m1.begin(), m1.end());
  first.insert(first.end(), m2.begin(), m2.begin() + 3);

  fh.on_bytes(first.data(), first.size());
  ASSERT_EQ(sink.events.size(), 1u);
  EXPECT_EQ(std::get<Add>(sink.events[0]).id, 100u);
  EXPECT_EQ(fh.msgs_parsed(), 1u);
  EXPECT_EQ(fh.bytes_consumed(), m1.size());
  EXPECT_EQ(fh.buffered(), 3u);

  // m2 剩下的
  fh.on_bytes(m2.data() + 3, m2.size() - 3);
  ASSERT_EQ(sink.events.size(), 2u);
  EXPECT_EQ(std::get<Add>(sink.events[1]).id, 101u);
  EXPECT_EQ(fh.msgs_parsed(), 2u);
  EXPECT_EQ(fh.bytes_consumed(), m1.size() + m2.size());
  EXPECT_EQ(fh.buffered(), 0u);
}

// ================================================================
// 5. 极端 fragmentation -- 一字节一字节地喂
// ================================================================
TEST(FeedHandler, OneByteAtATime) {
  CollectingSink sink;
  pipeline::FeedHandler<CollectingSink> fh(sink);

  auto all = concat({
      make_add(/*seq=*/1, /*ts=*/42, /*sym=*/1, /*id=*/100, Side::Buy,
               /*px=*/100, /*qty=*/5),
      make_cancel(/*seq=*/2, /*ts=*/42, /*sym=*/1, /*id=*/100),
      make_clear(/*seq=*/3, /*ts=*/42, /*sym=*/1),
      make_add(/*seq=*/4, /*ts=*/42, /*sym=*/1, /*id=*/101, Side::Sell,
               /*px=*/200, /*qty=*/10),
  });

  for (size_t i = 0; i < all.size(); ++i)
    fh.on_bytes(all.data() + i, 1);

  ASSERT_EQ(sink.events.size(), 4u);
  EXPECT_TRUE(std::holds_alternative<Add>(sink.events[0]));
  EXPECT_TRUE(std::holds_alternative<Cancel>(sink.events[1]));
  EXPECT_TRUE(std::holds_alternative<Clear>(sink.events[2]));
  EXPECT_TRUE(std::holds_alternative<Add>(sink.events[3]));
  EXPECT_EQ(fh.msgs_parsed(), 4u);
  EXPECT_EQ(fh.bytes_consumed(), all.size());
  EXPECT_EQ(fh.buffered(), 0u);
}

// ================================================================
// 6. 坏数据  --  一条完整 Add 后跟一条 msg_type 非法的 header
// ================================================================
TEST(FeedHandler, CorruptAfterGoodMessage) {
  CollectingSink sink;
  pipeline::FeedHandler<CollectingSink> fh(sink);

  auto good = make_add(/*seq=*/1, /*ts=*/42, /*sym=*/1, /*id=*/100, Side::Buy,
                       /*px=*/100, /*qty=*/5);

  // 非法 msg_type = 99, seq = 0
  // (enc_header 第二参是 seq, fixture 内部会把它写进 header)
  auto bad = enc_header(/*msg_type=*/99, /*seq=*/0).data;

  auto all = concat({good, bad});
  fh.on_bytes(all.data(), all.size());

  ASSERT_EQ(sink.events.size(), 1u);
  EXPECT_TRUE(std::holds_alternative<Add>(sink.events[0]));
  EXPECT_EQ(std::get<Add>(sink.events[0]).id, 100u);
  EXPECT_EQ(fh.msgs_parsed(), 1u);

  EXPECT_EQ(fh.corrupt_events(), 1u);
  EXPECT_EQ(fh.buffered(), 0u);
  EXPECT_GE(fh.bytes_consumed(), good.size());
}

// ================================================================
// 7. 空调用 -- 不该爆
// ================================================================
TEST(FeedHandler, EmptyCall) {
  CollectingSink sink;
  pipeline::FeedHandler<CollectingSink> fh(sink);

  fh.on_bytes(nullptr, 0);
  EXPECT_EQ(sink.events.size(), 0u);
  EXPECT_EQ(fh.msgs_parsed(), 0u);
  EXPECT_EQ(fh.buffered(), 0u);
}

// ================================================================
// 8. seq 透传, 即使被字节级分片
// ================================================================
TEST(FeedHandler, SeqPropagatesThroughReassembly) {
  SeqRecordingSink sink;
  pipeline::FeedHandler<SeqRecordingSink> fh(sink);

  auto f1 = make_add(/*seq=*/100, /*ts=*/1, /*sym=*/1, /*id=*/1, Side::Buy,
                     /*px=*/1'000'000, /*qty=*/10);
  auto f2 = make_cancel(/*seq=*/101, /*ts=*/2, /*sym=*/1, /*id=*/1);
  auto f3 = make_trade(/*seq=*/777, /*ts=*/3, /*sym=*/1,
                       /*px=*/1'000'500, /*qty=*/5, /*trade_id=*/42);

  auto stream = concat({f1, f2, f3});

  for (std::size_t i = 0; i < stream.size(); ++i) {
    fh.on_bytes(stream.data() + i, 1);
  }

  EXPECT_EQ(sink.seqs, (std::vector<std::uint64_t>{100u, 101u, 777u}));
  EXPECT_EQ(fh.msgs_parsed(), 3u);
  EXPECT_EQ(fh.corrupt_events(), 0u);
  EXPECT_EQ(fh.buffered(), 0u);
}