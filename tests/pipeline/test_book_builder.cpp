#include <cstddef>
#include <cstdint>
#include <hft/core/order_book.hpp>
#include <hft/md/md_event.hpp>
#include <hft/pipeline/book_builder.hpp>

#include <gtest/gtest.h>
#include <optional>
#include <vector>

using hft::Side;
using hft::pipeline::BookBuilder;
namespace md = hft::md;

namespace {

// 记录下游收到了什么, 以及收到时 book 的状态.
struct SpyStage {
  struct Call {
    std::uint64_t seq;
    md::MdEvent ev;
    struct Snap {
      std::optional<hft::Price> bid, ask;
      std::size_t n;
    };
    std::unordered_map<md::SymbolId, Snap> snap;
  };
  std::vector<Call> calls;

  void on_md(std::uint64_t seq, const md::MdEvent &e,
             const std::unordered_map<md::SymbolId, hft::core::OrderBook>
                 &books) noexcept {
    Call c{seq, e, {}};
    for (auto &[s, b] : books) {
      c.snap[s] = {b.best_bid(), b.best_ask(), b.num_orders()};
    }
    calls.push_back(std::move(c));
  }
};

using Builder = BookBuilder<SpyStage>;

// 小工具: 取 sym 的 book 和一些状态
auto &book_of(Builder &bb, md::SymbolId s) {
  // books() 返回 const&, 做 const_cast 只在测试里能接受, 或者加访问器.
  // 这里直接读 const 视图就够了, 不改.
  return bb.books().at(s);
}

constexpr md::SymbolId S1 = 1;
constexpr md::SymbolId S2 = 2;

} // namespace

// ────────────── 单消息 ──────────────

TEST(BookBuilder, AddCreatesBookAndRests) {
  Builder bb;
  bb.on_md(0, md::Add{.ts = 1,
                      .sym = S1,
                      .id = 1,
                      .side = Side::Buy,
                      .px = 100,
                      .qty = 10});

  ASSERT_EQ(bb.books().size(), 1u);
  const auto &b = book_of(bb, S1);
  EXPECT_EQ(b.best_bid(), std::optional<hft::Price>{100});
  EXPECT_FALSE(b.best_ask().has_value());
  EXPECT_EQ(b.qty_at(Side::Buy, 100), 10);
  EXPECT_EQ(b.num_orders(), 1u);
}

TEST(BookBuilder, CancelRemovesOrder) {
  Builder bb;
  bb.on_md(0, md::Add{.ts = 1,
                      .sym = S1,
                      .id = 1,
                      .side = Side::Buy,
                      .px = 100,
                      .qty = 10});
  bb.on_md(0, md::Cancel{.ts = 2, .sym = S1, .id = 1});

  const auto &b = book_of(bb, S1);
  EXPECT_FALSE(b.best_bid().has_value());
  EXPECT_EQ(b.num_orders(), 0u);
}

TEST(BookBuilder, ReduceShrinksQty) {
  Builder bb;
  bb.on_md(0, md::Add{.ts = 1,
                      .sym = S1,
                      .id = 1,
                      .side = Side::Buy,
                      .px = 100,
                      .qty = 10});
  bb.on_md(0, md::Reduce{.ts = 2, .sym = S1, .id = 1, .new_qty = 3});

  EXPECT_EQ(book_of(bb, S1).qty_at(Side::Buy, 100), 3);
}

TEST(BookBuilder, ExecPartial) {
  Builder bb;
  bb.on_md(0, md::Add{.ts = 1,
                      .sym = S1,
                      .id = 1,
                      .side = Side::Sell,
                      .px = 200,
                      .qty = 10});
  bb.on_md(0, md::Exec{.ts = 2,
                       .sym = S1,
                       .id = 1,
                       .exec_qty = 4,
                       .px = 200,
                       .trade_id = 1001});

  const auto &b = book_of(bb, S1);
  EXPECT_EQ(b.best_ask(), std::optional<hft::Price>{200});
  EXPECT_EQ(b.qty_at(Side::Sell, 200), 6);
}

TEST(BookBuilder, ExecFullRemoves) {
  Builder bb;
  bb.on_md(0, md::Add{.ts = 1,
                      .sym = S1,
                      .id = 1,
                      .side = Side::Sell,
                      .px = 200,
                      .qty = 10});
  bb.on_md(0, md::Exec{.ts = 2,
                       .sym = S1,
                       .id = 1,
                       .exec_qty = 10,
                       .px = 200,
                       .trade_id = 1002});

  const auto &b = book_of(bb, S1);
  EXPECT_FALSE(b.best_ask().has_value());
  EXPECT_EQ(b.num_orders(), 0u);
}

// ────────────── Replace ──────────────
// 前提: md::Replace 已加 Side side; apply(Replace) 里 o.side = r.side;

TEST(BookBuilder, ReplaceCancelsOldAddsNew) {
  Builder bb;
  bb.on_md(0, md::Add{.ts = 1,
                      .sym = S1,
                      .id = 1,
                      .side = Side::Buy,
                      .px = 100,
                      .qty = 10});
  bb.on_md(0, md::Replace{.ts = 2,
                          .sym = S1,
                          .old_id = 1,
                          .new_id = 2,
                          .side = Side::Buy,
                          .px = 101,
                          .qty = 7});

  const auto &b = book_of(bb, S1);
  EXPECT_EQ(b.best_bid(), std::optional<hft::Price>{101});
  EXPECT_EQ(b.qty_at(Side::Buy, 100), 0);
  EXPECT_EQ(b.qty_at(Side::Buy, 101), 7);
  EXPECT_EQ(b.num_orders(), 1u);
}

// ────────────── Trade / Clear ──────────────

TEST(BookBuilder, TradeDoesNotTouchBook) {
  Builder bb;
  bb.on_md(0, md::Add{.ts = 1,
                      .sym = S1,
                      .id = 1,
                      .side = Side::Sell,
                      .px = 200,
                      .qty = 10});
  bb.on_md(
      0, md::Trade{.ts = 2, .sym = S1, .px = 200, .qty = 5, .trade_id = 1003});

  const auto &b = book_of(bb, S1);
  EXPECT_EQ(b.best_ask(), std::optional<hft::Price>{200});
  EXPECT_EQ(b.qty_at(Side::Sell, 200), 10);
}

TEST(BookBuilder, ClearErasesBook) {
  Builder bb;
  bb.on_md(0, md::Add{.ts = 1,
                      .sym = S1,
                      .id = 1,
                      .side = Side::Buy,
                      .px = 100,
                      .qty = 10});
  bb.on_md(0, md::Clear{.ts = 2, .sym = S1});

  EXPECT_EQ(bb.books().count(S1), 0u);
}

// ────────────── 多 symbol 隔离 ──────────────

TEST(BookBuilder, PerSymbolIsolation) {
  Builder bb;
  bb.on_md(0, md::Add{.ts = 1,
                      .sym = S1,
                      .id = 1,
                      .side = Side::Buy,
                      .px = 100,
                      .qty = 10});
  bb.on_md(0, md::Add{.ts = 2,
                      .sym = S2,
                      .id = 2,
                      .side = Side::Sell,
                      .px = 200,
                      .qty = 5});

  ASSERT_EQ(bb.books().size(), 2u);
  EXPECT_EQ(book_of(bb, S1).best_bid(), std::optional<hft::Price>{100});
  EXPECT_FALSE(book_of(bb, S1).best_ask().has_value());
  EXPECT_EQ(book_of(bb, S2).best_ask(), std::optional<hft::Price>{200});
  EXPECT_FALSE(book_of(bb, S2).best_bid().has_value());

  bb.on_md(0, md::Clear{.ts = 3, .sym = S1});
  EXPECT_EQ(bb.books().count(S1), 0u);
  EXPECT_EQ(bb.books().count(S2), 1u); // S2 不受影响
}

TEST(BookBuilder, SameIdDifferentSymbolsDoNotCollide) {
  // 不同 sym 用同一个 id 应该互不影响 (index 是 per-book 的)
  Builder bb;
  bb.on_md(0, md::Add{.ts = 1,
                      .sym = S1,
                      .id = 42,
                      .side = Side::Buy,
                      .px = 100,
                      .qty = 10});
  bb.on_md(0, md::Add{.ts = 2,
                      .sym = S2,
                      .id = 42,
                      .side = Side::Buy,
                      .px = 300,
                      .qty = 20});
  bb.on_md(0, md::Cancel{.ts = 3, .sym = S1, .id = 42});

  EXPECT_EQ(book_of(bb, S1).num_orders(), 0u);
  EXPECT_EQ(book_of(bb, S2).num_orders(), 1u);
  EXPECT_EQ(book_of(bb, S2).best_bid(), std::optional<hft::Price>{300});
}

// ────────────── 下游 Stage 接线 ──────────────

TEST(BookBuilder, NextStageSeesEveryEventAndUpdatedBook) {
  Builder bb;
  auto &spy = bb.next();

  bb.on_md(0, md::Add{.ts = 1,
                      .sym = S1,
                      .id = 1,
                      .side = Side::Buy,
                      .px = 100,
                      .qty = 10});
  bb.on_md(0, md::Exec{.ts = 2,
                       .sym = S1,
                       .id = 1,
                       .exec_qty = 4,
                       .px = 100,
                       .trade_id = 1});

  ASSERT_EQ(spy.calls.size(), 2u);
  // 下游看到的 snapshot 应已包含当条事件的效果 (apply 先于 next_.on_md)
  EXPECT_EQ(spy.calls[0].snap.at(S1).bid, 100);
  EXPECT_EQ(spy.calls[0].snap.at(S1).n, 1u);
  EXPECT_EQ(spy.calls[1].snap.at(S1).bid, 100);
  EXPECT_EQ(spy.calls[1].snap.at(S1).n, 1u); // partial, 还在
}