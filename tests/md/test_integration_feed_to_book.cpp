#include "wire_fixtures.hpp"

#include <hft/md/feed_handler.hpp>
#include <hft/md/md_event.hpp>

#include <gtest/gtest.h>

#include <cstddef>
#include <vector>

using hft::Side;
using hft::test::concat;
using hft::test::make_add;
using hft::test::make_cancel;
using hft::test::make_reduce;

namespace {

struct CountingSink {
  std::size_t n = 0;
  void on_md(const hft::md::MdEvent & /*ev*/) { ++n; }
};

} // namespace

// -------------------------------------------------------------------
// 1. 所有 event 都应投递到 sink.
// -------------------------------------------------------------------
TEST(Integration, AllEventsDelivered) {
  auto bytes = concat({
      make_add(/*ts=*/1, /*sym=*/1, /*id=*/1, Side::Buy, 100000, 100),
      make_add(/*ts=*/2, /*sym=*/1, /*id=*/2, Side::Buy, 99900, 200),
      make_add(/*ts=*/3, /*sym=*/1, /*id=*/3, Side::Sell, 100100, 150),
  });

  CountingSink sink;
  hft::md::FeedHandler<CountingSink> fh(sink);
  fh.on_bytes(bytes.data(), bytes.size());

  EXPECT_EQ(sink.n, 3u);
  EXPECT_EQ(fh.msgs_parsed(), 3u);
  EXPECT_EQ(fh.corrupt_events(), 0u);
  EXPECT_EQ(fh.buffered(), 0u);
}

// -------------------------------------------------------------------
// 2. 把同一串字节一次全喂 vs 一字节一字节喂, 结果必须完全一致.
//    这是 FeedHandler 分片缓冲逻辑的核心契约.
// -------------------------------------------------------------------
TEST(Integration, SplitChunksProduceSameState) {
  auto bytes = concat({
      make_add(/*ts=*/1, /*sym=*/1, /*id=*/1, Side::Buy, 100000, 100),
      make_add(/*ts=*/2, /*sym=*/1, /*id=*/2, Side::Sell, 100100, 150),
      make_reduce(/*ts=*/3, /*sym=*/1, /*id=*/2, /*new_qty=*/50),
      make_cancel(/*ts=*/4, /*sym=*/1, /*id=*/1),
  });

  CountingSink whole_sink;
  hft::md::FeedHandler<CountingSink> whole_fh(whole_sink);
  whole_fh.on_bytes(bytes.data(), bytes.size());

  CountingSink drip_sink;
  hft::md::FeedHandler<CountingSink> drip_fh(drip_sink);
  for (const auto &b : bytes)
    drip_fh.on_bytes(&b, 1);

  EXPECT_EQ(whole_sink.n, 4u);
  EXPECT_EQ(drip_sink.n, whole_sink.n);
  EXPECT_EQ(drip_fh.msgs_parsed(), whole_fh.msgs_parsed());
  EXPECT_EQ(drip_fh.buffered(), 0u);
}

// -------------------------------------------------------------------
// 3. 任意 2-way 切分点都必须等价于一次性喂入.
// -------------------------------------------------------------------
TEST(Integration, ArbitrarySplitPointYieldsSameCount) {
  auto bytes = concat({
      make_add(/*ts=*/1, /*sym=*/1, /*id=*/1, Side::Buy, 100000, 100),
      make_add(/*ts=*/2, /*sym=*/1, /*id=*/2, Side::Sell, 100100, 150),
  });

  for (std::size_t cut = 0; cut <= bytes.size(); ++cut) {
    CountingSink sink;
    hft::md::FeedHandler<CountingSink> fh(sink);
    fh.on_bytes(bytes.data(), cut);
    fh.on_bytes(bytes.data() + cut, bytes.size() - cut);

    EXPECT_EQ(sink.n, 2u) << "cut=" << cut;
    EXPECT_EQ(fh.buffered(), 0u) << "cut=" << cut;
  }
}

// -------------------------------------------------------------------
// 4. 一段垃圾字节必须被丢弃 (corrupt_events++), 但不能破坏之前已投递
//    的事件, 并且恢复后仍能解析后续合法帧.
// -------------------------------------------------------------------
TEST(Integration, CorruptFrameDoesNotBreakPriorOrSubsequent) {
  auto good1 = make_add(/*ts=*/1, /*sym=*/1, /*id=*/1, Side::Buy, 100000, 100);
  std::vector<std::byte> garbage(16, std::byte{0xFF}); // bogus msg type

  CountingSink sink;
  hft::md::FeedHandler<CountingSink> fh(sink);

  fh.on_bytes(good1.data(), good1.size());
  EXPECT_EQ(sink.n, 1u);
  EXPECT_EQ(fh.corrupt_events(), 0u);

  fh.on_bytes(garbage.data(), garbage.size());
  EXPECT_EQ(fh.corrupt_events(), 1u);
  EXPECT_EQ(sink.n, 1u); // 不应少也不应多

  auto good2 = make_add(/*ts=*/2, /*sym=*/1, /*id=*/2, Side::Sell, 100100, 50);
  fh.on_bytes(good2.data(), good2.size());
  EXPECT_EQ(sink.n, 2u);
}

// -------------------------------------------------------------------
// 5. NeedMore: 给一个不完整的 frame, sink 不应收到任何事件, 字节应
//    残留在 buf_ 里等下次.
// -------------------------------------------------------------------
TEST(Integration, TruncatedFrameBuffersUntilComplete) {
  auto frame = make_add(/*ts=*/1, /*sym=*/1, /*id=*/1, Side::Buy, 100000, 100);

  CountingSink sink;
  hft::md::FeedHandler<CountingSink> fh(sink);

  // 先喂前一半
  std::size_t half = frame.size() / 2;
  fh.on_bytes(frame.data(), half);
  EXPECT_EQ(sink.n, 0u);
  EXPECT_EQ(fh.buffered(), half);

  // 喂剩下的, 才应产出 1 条事件
  fh.on_bytes(frame.data() + half, frame.size() - half);
  EXPECT_EQ(sink.n, 1u);
  EXPECT_EQ(fh.buffered(), 0u);
}