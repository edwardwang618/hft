// tests/pipeline/test_feed_handler.cpp
#include "fix_fixtures.hpp"
#include "hft/core/types.hpp"
#include "wire_fixtures.hpp"
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <gtest/gtest.h>
#include <hft/md/binary_parser.hpp>
#include <hft/md/fix_parser.hpp>
#include <hft/md/wire_binary.hpp>
#include <hft/pipeline/feed_handler.hpp>
#include <string>
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
  pipeline::FeedHandler<CollectingSink, BinaryParser> fh(sink);

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
  pipeline::FeedHandler<CollectingSink, BinaryParser> fh(sink);

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
  pipeline::FeedHandler<CollectingSink, BinaryParser> fh(sink);

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
  pipeline::FeedHandler<CollectingSink, BinaryParser> fh(sink);

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
  pipeline::FeedHandler<CollectingSink, BinaryParser> fh(sink);

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
  pipeline::FeedHandler<CollectingSink, BinaryParser> fh(sink);

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
  pipeline::FeedHandler<CollectingSink, BinaryParser> fh(sink);

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
  pipeline::FeedHandler<SeqRecordingSink, BinaryParser> fh(sink);

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

// ================================================================
// FIX parser variants
// ================================================================

namespace {
std::vector<std::byte> fix_bytes(const std::string &s) {
  auto *p = reinterpret_cast<const std::byte *>(s.data());
  return {p, p + s.size()};
}
} // namespace

TEST(FeedHandlerFix, SingleCompleteMessage) {
  CollectingSink sink;
  pipeline::FeedHandler<CollectingSink, FixParser> fh(sink);

  auto b = fix_bytes(test::encode_fix(md::Add{1000, 42, 7, Side::Buy, 12345, 10}, 1));
  fh.on_bytes(b.data(), b.size());

  ASSERT_EQ(sink.events.size(), 1u);
  ASSERT_TRUE(std::holds_alternative<md::Add>(sink.events[0]));
  const auto &a = std::get<md::Add>(sink.events[0]);
  EXPECT_EQ(a.ts, 1000u);
  EXPECT_EQ(a.sym, 42u);
  EXPECT_EQ(a.id, 7u);
  EXPECT_EQ(a.side, Side::Buy);
  EXPECT_EQ(a.px, 12345);
  EXPECT_EQ(a.qty, 10);
  EXPECT_EQ(fh.msgs_parsed(), 1u);
  EXPECT_EQ(fh.buffered(), 0u);
}

TEST(FeedHandlerFix, SplitAcrossTwoChunks) {
  CollectingSink sink;
  pipeline::FeedHandler<CollectingSink, FixParser> fh(sink);

  auto b = fix_bytes(test::encode_fix(md::Add{1000, 42, 7, Side::Buy, 12345, 10}, 1));
  ASSERT_GT(b.size(), 1u);

  fh.on_bytes(b.data(), b.size() / 2);
  EXPECT_EQ(sink.events.size(), 0u);
  EXPECT_EQ(fh.msgs_parsed(), 0u);
  EXPECT_GT(fh.buffered(), 0u);

  fh.on_bytes(b.data() + b.size() / 2, b.size() - b.size() / 2);
  ASSERT_EQ(sink.events.size(), 1u);
  EXPECT_TRUE(std::holds_alternative<md::Add>(sink.events[0]));
  EXPECT_EQ(fh.msgs_parsed(), 1u);
  EXPECT_EQ(fh.buffered(), 0u);
}

TEST(FeedHandlerFix, ThreeMessagesInOneChunk) {
  CollectingSink sink;
  pipeline::FeedHandler<CollectingSink, FixParser> fh(sink);

  std::string buf;
  buf += test::encode_fix(md::Add{42, 1, 100, Side::Buy, 100, 5}, 1);
  buf += test::encode_fix(md::Cancel{42, 1, 100}, 2);
  buf += test::encode_fix(md::Add{42, 1, 101, Side::Sell, 200, 10}, 3);
  auto b = fix_bytes(buf);
  fh.on_bytes(b.data(), b.size());

  ASSERT_EQ(sink.events.size(), 3u);
  EXPECT_TRUE(std::holds_alternative<md::Add>(sink.events[0]));
  EXPECT_TRUE(std::holds_alternative<md::Cancel>(sink.events[1]));
  EXPECT_TRUE(std::holds_alternative<md::Add>(sink.events[2]));
  EXPECT_EQ(fh.msgs_parsed(), 3u);
  EXPECT_EQ(fh.buffered(), 0u);
}

TEST(FeedHandlerFix, OneFullPlusPartial) {
  CollectingSink sink;
  pipeline::FeedHandler<CollectingSink, FixParser> fh(sink);

  auto s1 = test::encode_fix(md::Add{42, 1, 100, Side::Buy, 100, 5}, 1);
  auto s2 = test::encode_fix(md::Add{42, 1, 101, Side::Buy, 200, 10}, 2);

  // m1 complete + first half of m2
  std::string first = s1 + s2.substr(0, s2.size() / 2);
  auto b1 = fix_bytes(first);
  fh.on_bytes(b1.data(), b1.size());
  ASSERT_EQ(sink.events.size(), 1u);
  EXPECT_EQ(std::get<md::Add>(sink.events[0]).id, 100u);
  EXPECT_EQ(fh.msgs_parsed(), 1u);
  EXPECT_GT(fh.buffered(), 0u);

  auto b2 = fix_bytes(s2.substr(s2.size() / 2));
  fh.on_bytes(b2.data(), b2.size());
  ASSERT_EQ(sink.events.size(), 2u);
  EXPECT_EQ(std::get<md::Add>(sink.events[1]).id, 101u);
  EXPECT_EQ(fh.msgs_parsed(), 2u);
  EXPECT_EQ(fh.buffered(), 0u);
}

TEST(FeedHandlerFix, OneByteAtATime) {
  CollectingSink sink;
  pipeline::FeedHandler<CollectingSink, FixParser> fh(sink);

  std::string buf;
  buf += test::encode_fix(md::Add{42, 1, 100, Side::Buy, 100, 5}, 1);
  buf += test::encode_fix(md::Cancel{42, 1, 100}, 2);
  buf += test::encode_fix(md::Clear{42, 1}, 3);
  buf += test::encode_fix(md::Add{42, 1, 101, Side::Sell, 200, 10}, 4);
  auto b = fix_bytes(buf);

  for (std::size_t i = 0; i < b.size(); ++i)
    fh.on_bytes(b.data() + i, 1);

  ASSERT_EQ(sink.events.size(), 4u);
  EXPECT_TRUE(std::holds_alternative<md::Add>(sink.events[0]));
  EXPECT_TRUE(std::holds_alternative<md::Cancel>(sink.events[1]));
  EXPECT_TRUE(std::holds_alternative<md::Clear>(sink.events[2]));
  EXPECT_TRUE(std::holds_alternative<md::Add>(sink.events[3]));
  EXPECT_EQ(fh.msgs_parsed(), 4u);
  EXPECT_EQ(fh.buffered(), 0u);
}

TEST(FeedHandlerFix, CorruptAfterGoodMessage) {
  CollectingSink sink;
  pipeline::FeedHandler<CollectingSink, FixParser> fh(sink);

  auto good = fix_bytes(test::encode_fix(md::Add{42, 1, 100, Side::Buy, 100, 5}, 1));
  auto bad  = fix_bytes(std::string{"35=Z|34=99|52=1|55=1\n"}); // unknown msg type

  fh.on_bytes(good.data(), good.size());
  fh.on_bytes(bad.data(), bad.size());

  ASSERT_EQ(sink.events.size(), 1u);
  EXPECT_TRUE(std::holds_alternative<md::Add>(sink.events[0]));
  EXPECT_EQ(fh.msgs_parsed(), 1u);
  EXPECT_EQ(fh.corrupt_events(), 1u);
  EXPECT_EQ(fh.buffered(), 0u);
}

TEST(FeedHandlerFix, SeqPropagatesThroughReassembly) {
  SeqRecordingSink sink;
  pipeline::FeedHandler<SeqRecordingSink, FixParser> fh(sink);

  std::string buf;
  buf += test::encode_fix(md::Add{1, 1, 1, Side::Buy, 1'000'000, 10}, 100);
  buf += test::encode_fix(md::Cancel{2, 1, 1}, 101);
  buf += test::encode_fix(md::Trade{3, 1, 1'000'500, 5, 42}, 777);
  auto b = fix_bytes(buf);

  for (std::size_t i = 0; i < b.size(); ++i)
    fh.on_bytes(b.data() + i, 1);

  EXPECT_EQ(sink.seqs, (std::vector<std::uint64_t>{100u, 101u, 777u}));
  EXPECT_EQ(fh.msgs_parsed(), 3u);
  EXPECT_EQ(fh.corrupt_events(), 0u);
  EXPECT_EQ(fh.buffered(), 0u);
}