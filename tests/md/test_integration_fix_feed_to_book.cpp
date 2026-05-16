#include "fix_fixtures.hpp"

#include <hft/core/map_order_book.hpp>
#include <hft/md/fix_parser.hpp>
#include <hft/md/md_event.hpp>
#include <hft/pipeline/book_builder.hpp>
#include <hft/pipeline/feed_handler.hpp>

#include <gtest/gtest.h>

#include <cstddef>
#include <optional>
#include <string>
#include <unordered_map>

using hft::Side;
using hft::test::encode_fix;
using namespace hft::md;

namespace {

struct NullTerminal {
  void on_md(std::uint64_t, const MdEvent &,
             const std::unordered_map<SymbolId, hft::core::MapOrderBook<>>
                 &) noexcept {}
};

using Pipe = hft::pipeline::BookBuilder<NullTerminal>;
using FixFeedHandler = hft::pipeline::FeedHandler<Pipe, FixParser>;

const hft::core::MapOrderBook<> &book_at(const Pipe &p, SymbolId s) {
  return p.books().at(s);
}

std::vector<std::byte> to_bytes(const std::string &s) {
  auto *p = reinterpret_cast<const std::byte *>(s.data());
  return {p, p + s.size()};
}

} // namespace

TEST(FixFeedToBook, MultipleAddsReflectInBook) {
  std::string buf;
  buf += encode_fix(Add{1, 1, 101, Side::Buy, 100000, 100}, 1);
  buf += encode_fix(Add{2, 1, 102, Side::Buy, 99900, 200}, 2);
  buf += encode_fix(Add{3, 1, 201, Side::Sell, 100100, 150}, 3);
  buf += encode_fix(Add{4, 1, 202, Side::Sell, 100200, 50}, 4);
  auto bytes = to_bytes(buf);

  Pipe pipe;
  FixFeedHandler fh(pipe);
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

TEST(FixFeedToBook, AddReduceCancelLifecycle) {
  std::string buf;
  buf += encode_fix(Add{1, 1, 1, Side::Buy, 100000, 100}, 1);
  buf += encode_fix(Reduce{2, 1, 1, 40}, 2);
  buf += encode_fix(Cancel{3, 1, 1}, 3);
  auto bytes = to_bytes(buf);

  Pipe pipe;
  FixFeedHandler fh(pipe);
  fh.on_bytes(bytes.data(), bytes.size());

  EXPECT_EQ(fh.msgs_parsed(), 3u);
  EXPECT_EQ(fh.corrupt_events(), 0u);

  const auto &b = book_at(pipe, 1);
  EXPECT_FALSE(b.best_bid().has_value());
  EXPECT_FALSE(b.best_ask().has_value());
  EXPECT_EQ(b.num_orders(), 0u);
}

TEST(FixFeedToBook, ChunkingDoesNotAffectFinalBookState) {
  std::string buf;
  buf += encode_fix(Add{1, 1, 1, Side::Buy, 100000, 100}, 1);
  buf += encode_fix(Add{2, 1, 2, Side::Sell, 100100, 150}, 2);
  buf += encode_fix(Reduce{3, 1, 2, 50}, 3);
  buf += encode_fix(Add{4, 1, 3, Side::Buy, 99900, 200}, 4);
  buf += encode_fix(Cancel{5, 1, 1}, 5);
  auto bytes = to_bytes(buf);

  Pipe whole;
  FixFeedHandler whole_fh(whole);
  whole_fh.on_bytes(bytes.data(), bytes.size());

  Pipe drip;
  FixFeedHandler drip_fh(drip);
  for (const auto &b : bytes)
    drip_fh.on_bytes(&b, 1);

  const auto &wb = book_at(whole, 1);
  const auto &db = book_at(drip, 1);

  EXPECT_EQ(wb.best_bid(), db.best_bid());
  EXPECT_EQ(wb.best_ask(), db.best_ask());
  EXPECT_EQ(wb.num_orders(), db.num_orders());
  EXPECT_EQ(wb.qty_at(Side::Buy, 99900), db.qty_at(Side::Buy, 99900));
  EXPECT_EQ(wb.qty_at(Side::Sell, 100100), db.qty_at(Side::Sell, 100100));

  EXPECT_EQ(wb.best_bid(), std::optional<hft::Price>{99900});
  EXPECT_EQ(wb.best_ask(), std::optional<hft::Price>{100100});
  EXPECT_EQ(wb.qty_at(Side::Sell, 100100), 50);
  EXPECT_EQ(wb.num_orders(), 2u);
}

TEST(FixFeedToBook, GarbageInMiddleDoesNotCorruptBook) {
  auto pre = to_bytes(encode_fix(Add{1, 1, 1, Side::Buy, 100000, 100}, 1));
  auto post = to_bytes(encode_fix(Add{2, 1, 2, Side::Sell, 100100, 150}, 2));
  std::string garbage = "35=Z|34=99|52=1|55=1\n"; // unknown msg type
  auto bad = to_bytes(garbage);

  Pipe pipe;
  FixFeedHandler fh(pipe);
  fh.on_bytes(pre.data(), pre.size());
  fh.on_bytes(bad.data(), bad.size());
  fh.on_bytes(post.data(), post.size());

  EXPECT_GE(fh.corrupt_events(), 1u);
  EXPECT_EQ(fh.msgs_parsed(), 2u);

  const auto &b = book_at(pipe, 1);
  EXPECT_EQ(b.best_bid(), std::optional<hft::Price>{100000});
  EXPECT_EQ(b.best_ask(), std::optional<hft::Price>{100100});
  EXPECT_EQ(b.num_orders(), 2u);
}

TEST(FixFeedToBook, MultiSymbolIsolation) {
  std::string buf;
  buf += encode_fix(Add{1, 1, 11, Side::Buy, 100000, 100}, 1);
  buf += encode_fix(Add{2, 2, 21, Side::Sell, 200000, 50}, 2);
  buf += encode_fix(Add{3, 1, 12, Side::Sell, 100100, 80}, 3);
  buf += encode_fix(Add{4, 2, 22, Side::Buy, 199900, 30}, 4);
  auto bytes = to_bytes(buf);

  Pipe pipe;
  FixFeedHandler fh(pipe);
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
