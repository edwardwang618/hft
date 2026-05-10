#include "wire_fixtures.hpp"

#include <cstdint>
#include <hft/core/order_book.hpp>
#include <hft/pipeline/feed_handler.hpp>
#include <hft/md/md_event.hpp>
#include <hft/pipeline/book_builder.hpp>

#include <gtest/gtest.h>

#include <cstddef>
#include <optional>
#include <unordered_map>
#include <vector>

using hft::Side;
using hft::test::concat;
using hft::test::make_add;
using hft::test::make_cancel;
using hft::test::make_reduce;

namespace {

// 终端 stage: BookBuilder 的 Next 要求 on_md(ev, books), 这里空实现.
struct NullTerminal {
  void on_md(std::uint64_t, const hft::md::MdEvent &,
             const std::unordered_map<hft::md::SymbolId, hft::core::OrderBook>
                 &) noexcept {}
};

using Pipe = hft::pipeline::BookBuilder<NullTerminal>;

const hft::core::OrderBook &book_at(const Pipe &p, hft::md::SymbolId s) {
  return p.books().at(s);
}

} // namespace

// 1. 端到端: 一段合法字节 → parser → feed handler → book builder → OrderBook.
//    book 最终状态应反映最佳买卖价和各档数量.
TEST(FeedToBook, MultipleAddsReflectInBook) {
  auto bytes = concat({
      make_add(0, 1, 1, 101, Side::Buy, 100000, 100),
      make_add(0, 2, 1, 102, Side::Buy, 99900, 200),
      make_add(0, 3, 1, 201, Side::Sell, 100100, 150),
      make_add(0, 4, 1, 202, Side::Sell, 100200, 50),
  });

  Pipe pipe;
  hft::md::FeedHandler<Pipe> fh(pipe);
  fh.on_bytes(bytes.data(), bytes.size());

  EXPECT_EQ(fh.msgs_parsed(), 4u);
  EXPECT_EQ(fh.corrupt_events(), 0u);
  EXPECT_EQ(fh.buffered(), 0u);

  const auto &b = book_at(pipe, 1);
  EXPECT_EQ(b.best_bid(), std::optional<hft::Price>{100000});
  EXPECT_EQ(b.best_ask(), std::optional<hft::Price>{100100});
  EXPECT_EQ(b.qty_at(Side::Buy, 100000), 100);
  EXPECT_EQ(b.qty_at(Side::Buy, 99900), 200);
  EXPECT_EQ(b.qty_at(Side::Sell, 100100), 150);
  EXPECT_EQ(b.qty_at(Side::Sell, 100200), 50);
  EXPECT_EQ(b.num_orders(), 4u);
}

// 2. 生命周期: Add → Reduce → Cancel, book 最终为空.
TEST(FeedToBook, AddReduceCancelLifecycle) {
  auto bytes = concat({
      make_add(0, 1, 1, 1, Side::Buy, 100000, 100),
      make_reduce(0, 2, 1, 1, /*new_qty=*/40),
      make_cancel(0, 3, 1, 1),
  });

  Pipe pipe;
  hft::md::FeedHandler<Pipe> fh(pipe);
  fh.on_bytes(bytes.data(), bytes.size());

  EXPECT_EQ(fh.msgs_parsed(), 3u);
  EXPECT_EQ(fh.corrupt_events(), 0u);

  const auto &b = book_at(pipe, 1);
  EXPECT_FALSE(b.best_bid().has_value());
  EXPECT_FALSE(b.best_ask().has_value());
  EXPECT_EQ(b.num_orders(), 0u);
}

// 3. 分片等价性: 一次性 vs 逐字节, book 最终状态必须完全一致.
//    这是 feed handler 分片语义对下游 book state 的契约.
TEST(FeedToBook, ChunkingDoesNotAffectFinalBookState) {
  auto bytes = concat({
      make_add(0, 1, 1, 1, Side::Buy, 100000, 100),
      make_add(0, 2, 1, 2, Side::Sell, 100100, 150),
      make_reduce(0, 3, 1, 2, /*new_qty=*/50),
      make_add(0, 4, 1, 3, Side::Buy, 99900, 200),
      make_cancel(0, 5, 1, 1),
  });

  Pipe whole;
  hft::md::FeedHandler<Pipe> whole_fh(whole);
  whole_fh.on_bytes(bytes.data(), bytes.size());

  Pipe drip;
  hft::md::FeedHandler<Pipe> drip_fh(drip);
  for (const auto &b : bytes)
    drip_fh.on_bytes(&b, 1);

  const auto &wb = book_at(whole, 1);
  const auto &db = book_at(drip, 1);

  EXPECT_EQ(wb.best_bid(), db.best_bid());
  EXPECT_EQ(wb.best_ask(), db.best_ask());
  EXPECT_EQ(wb.num_orders(), db.num_orders());
  EXPECT_EQ(wb.qty_at(Side::Buy, 99900), db.qty_at(Side::Buy, 99900));
  EXPECT_EQ(wb.qty_at(Side::Sell, 100100), db.qty_at(Side::Sell, 100100));

  // 顺便做一次绝对值断言, 保证两边不是"同样错"
  EXPECT_EQ(wb.best_bid(), std::optional<hft::Price>{99900});
  EXPECT_EQ(wb.best_ask(), std::optional<hft::Price>{100100});
  EXPECT_EQ(wb.qty_at(Side::Sell, 100100), 50);
  EXPECT_EQ(wb.num_orders(), 2u); // id=1 已撤, id=2/3 留存
}

// 4. 中间塞垃圾: 不能污染 book, 前后合法帧都要落 book, corrupt 计数 >= 1.
TEST(FeedToBook, GarbageInMiddleDoesNotCorruptBook) {
  auto pre = make_add(0, 1, 1, 1, Side::Buy, 100000, 100);
  std::vector<std::byte> garbage(32, std::byte{0xFF});
  auto post = make_add(0, 2, 1, 2, Side::Sell, 100100, 150);

  Pipe pipe;
  hft::md::FeedHandler<Pipe> fh(pipe);
  fh.on_bytes(pre.data(), pre.size());
  fh.on_bytes(garbage.data(), garbage.size());
  fh.on_bytes(post.data(), post.size());

  EXPECT_GE(fh.corrupt_events(), 1u);
  EXPECT_EQ(fh.msgs_parsed(), 2u);

  const auto &b = book_at(pipe, 1);
  EXPECT_EQ(b.best_bid(), std::optional<hft::Price>{100000});
  EXPECT_EQ(b.best_ask(), std::optional<hft::Price>{100100});
  EXPECT_EQ(b.qty_at(Side::Buy, 100000), 100);
  EXPECT_EQ(b.qty_at(Side::Sell, 100100), 150);
  EXPECT_EQ(b.num_orders(), 2u);
}

// 5. 多 symbol 隔离: 混流输入, 两个 book 各自独立.
TEST(FeedToBook, MultiSymbolIsolation) {
  auto bytes = concat({
      make_add(0, 1, 1, 11, Side::Buy, 100000, 100),
      make_add(0, 2, 2, 21, Side::Sell, 200000, 50),
      make_add(0, 3, 1, 12, Side::Sell, 100100, 80),
      make_add(0, 4, 2, 22, Side::Buy, 199900, 30),
  });

  Pipe pipe;
  hft::md::FeedHandler<Pipe> fh(pipe);
  fh.on_bytes(bytes.data(), bytes.size());

  ASSERT_EQ(pipe.books().size(), 2u);
  const auto &b1 = book_at(pipe, 1);
  const auto &b2 = book_at(pipe, 2);

  EXPECT_EQ(b1.best_bid(), std::optional<hft::Price>{100000});
  EXPECT_EQ(b1.best_ask(), std::optional<hft::Price>{100100});
  EXPECT_EQ(b2.best_bid(), std::optional<hft::Price>{199900});
  EXPECT_EQ(b2.best_ask(), std::optional<hft::Price>{200000});
  EXPECT_EQ(b1.num_orders(), 2u);
  EXPECT_EQ(b2.num_orders(), 2u);
}