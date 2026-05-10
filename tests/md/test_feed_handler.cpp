// tests/md/test_feed_handler.cpp
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <gtest/gtest.h>
#include <hft/md/feed_handler.hpp>
#include <hft/md/wire_binary.hpp>
#include <span>
#include <variant>
#include <vector>

using namespace hft;
using namespace hft::md;

namespace {

// ---- 跟 test_binary_parser 里同样的 Buf helper ----
struct Buf {
  std::vector<std::byte> data;
  template <class T> void put(const T &v) {
    const auto *p = reinterpret_cast<const std::byte *>(&v);
    data.insert(data.end(), p, p + sizeof(T));
  }
  size_t size() const { return data.size(); }
  void patch_len() {
    uint16_t len = static_cast<uint16_t>(data.size());
    std::memcpy(data.data() + 2, &len, sizeof(len));
  }
};

Buf enc_header(uint8_t type) {
  Buf b;
  wire::Header h{};
  h.msg_type = type;
  h.msg_len = 0; // patched later
  h.seq = 0;
  b.put(h);
  return b;
}

// 造一条完整的 Add message
Buf make_add(uint64_t ts, uint32_t sym, uint64_t id, uint8_t side, int64_t px,
             int64_t qty) {
  auto b = enc_header(wire::kAdd);
  b.put<uint64_t>(ts);
  b.put<uint32_t>(sym);
  b.put<uint64_t>(id);
  b.put<uint8_t>(side);
  b.put<int64_t>(px);
  b.put<int64_t>(qty);
  b.patch_len();
  return b;
}

Buf make_cancel(uint64_t ts, uint32_t sym, uint64_t id) {
  auto b = enc_header(wire::kCancel);
  b.put<uint64_t>(ts);
  b.put<uint32_t>(sym);
  b.put<uint64_t>(id);
  b.patch_len();
  return b;
}

Buf make_clear(uint64_t ts, uint32_t sym) {
  auto b = enc_header(wire::kClear);
  b.put<uint64_t>(ts);
  b.put<uint32_t>(sym);
  b.patch_len();
  return b;
}

// 拼接多条 message
std::vector<std::byte> concat(std::initializer_list<Buf> bufs) {
  std::vector<std::byte> out;
  for (auto &b : bufs)
    out.insert(out.end(), b.data.begin(), b.data.end());
  return out;
}

// collecting sink
struct CollectingSink {
  std::vector<MdEvent> events;
  void on_md(std::uint64_t /*seq*/, const MdEvent &e) { events.push_back(e); }
};

} // namespace

// ================================================================
// 1. 一次喂完整 1 条 -- fast path
// ================================================================
TEST(FeedHandler, SingleCompleteMessage) {
  CollectingSink sink;
  FeedHandler<CollectingSink> fh(sink);

  auto b = make_add(/*ts*/ 1000, /*sym*/ 42, /*id*/ 7,
                    /*side*/ 0, /*px*/ 12345, /*qty*/ 10);

  fh.on_bytes(b.data.data(), b.size());

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
  FeedHandler<CollectingSink> fh(sink);

  auto b = make_add(1000, 42, 7, 0, 12345, 10);
  ASSERT_GT(b.size(), 1u);

  // 第一段: 1 byte (连 header 都不完整)
  fh.on_bytes(b.data.data(), 1);
  EXPECT_EQ(sink.events.size(), 0u);
  EXPECT_EQ(fh.msgs_parsed(), 0u);
  EXPECT_EQ(fh.buffered(), 1u);

  // 第二段: 剩下全部
  fh.on_bytes(b.data.data() + 1, b.size() - 1);
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
  FeedHandler<CollectingSink> fh(sink);

  auto all = concat({
      make_add(1, 42, 100, 0, 100, 5),
      make_cancel(2, 42, 100),
      make_add(3, 42, 101, 1, 200, 10),
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
//    验证: 产出 1 event, 剩余字节留在 buf 里; 补齐后产出第 2 条.
// ================================================================
TEST(FeedHandler, OneFullPlusPartial) {
  CollectingSink sink;
  FeedHandler<CollectingSink> fh(sink);

  auto m1 = make_add(1, 42, 100, 0, 100, 5);
  auto m2 = make_add(2, 42, 101, 0, 200, 10);
  ASSERT_GT(m2.size(), 3u);

  // m1 完整 + m2 前 3 字节
  std::vector<std::byte> first;
  first.insert(first.end(), m1.data.begin(), m1.data.end());
  first.insert(first.end(), m2.data.begin(), m2.data.begin() + 3);

  fh.on_bytes(first.data(), first.size());
  ASSERT_EQ(sink.events.size(), 1u);
  EXPECT_EQ(std::get<Add>(sink.events[0]).id, 100u);
  EXPECT_EQ(fh.msgs_parsed(), 1u);
  EXPECT_EQ(fh.bytes_consumed(), m1.size());
  EXPECT_EQ(fh.buffered(), 3u);

  // m2 剩下的
  fh.on_bytes(m2.data.data() + 3, m2.size() - 3);
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
  FeedHandler<CollectingSink> fh(sink);

  auto all = concat({
      make_add(1, 42, 100, 0, 100, 5),
      make_cancel(2, 42, 100),
      make_clear(3, 42),
      make_add(4, 42, 101, 1, 200, 10),
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
//    验证: 前面的 event 正确产出, corrupt 计数 +1, buf 被清空.
// ================================================================
TEST(FeedHandler, CorruptAfterGoodMessage) {
  CollectingSink sink;
  FeedHandler<CollectingSink> fh(sink);

  auto good = make_add(1, 42, 100, 0, 100, 5);

  // 非法 msg_type = 99, msg_len = sizeof(header) = 4
  auto bad = enc_header(99);
  bad.patch_len();

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
  FeedHandler<CollectingSink> fh(sink);

  fh.on_bytes(nullptr, 0);
  EXPECT_EQ(sink.events.size(), 0u);
  EXPECT_EQ(fh.msgs_parsed(), 0u);
  EXPECT_EQ(fh.buffered(), 0u);
}