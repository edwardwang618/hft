#include "../support/fix_fixtures.hpp"

#include <cstddef>
#include <cstring>
#include <gtest/gtest.h>
#include <hft/md/fix_parser.hpp>
#include <span>
#include <string>
#include <string_view>

using namespace hft;
using namespace hft::md;

namespace {

std::span<const std::byte> as_bytes(std::string_view s) {
  return {reinterpret_cast<const std::byte *>(s.data()), s.size()};
}

template <class T> const T &get_event(const ParseResult &r) {
  EXPECT_EQ(r.status, ParseResult::Ok);
  return std::get<T>(*r.event);
}

} // namespace

// ---------- Single-message round-trips ---------------------------------------

TEST(FixParser, AddRoundTrip) {
  Add a{1700000000ULL, 42, 100, Side::Buy, 10000, 5};
  std::string line = hft::test::encode_fix(a, 7);
  FixParser p;
  auto r = p.parse(as_bytes(line));
  ASSERT_EQ(r.status, ParseResult::Ok);
  EXPECT_EQ(r.consumed, line.size());
  EXPECT_EQ(r.seq, 7u);
  const auto &got = get_event<Add>(r);
  EXPECT_EQ(got.ts, a.ts);
  EXPECT_EQ(got.sym, a.sym);
  EXPECT_EQ(got.id, a.id);
  EXPECT_EQ(got.side, a.side);
  EXPECT_EQ(got.px, a.px);
  EXPECT_EQ(got.qty, a.qty);
}

TEST(FixParser, CancelRoundTrip) {
  Cancel c{1700000001ULL, 42, 100};
  auto line = hft::test::encode_fix(c, 8);
  FixParser p;
  auto r = p.parse(as_bytes(line));
  const auto &g = get_event<Cancel>(r);
  EXPECT_EQ(g.ts, c.ts);
  EXPECT_EQ(g.sym, c.sym);
  EXPECT_EQ(g.id, c.id);
}

TEST(FixParser, ReduceRoundTrip) {
  Reduce e{1700000002ULL, 42, 100, 3};
  auto line = hft::test::encode_fix(e, 9);
  FixParser p;
  auto r = p.parse(as_bytes(line));
  const auto &g = get_event<Reduce>(r);
  EXPECT_EQ(g.new_qty, 3);
}

TEST(FixParser, ExecRoundTrip) {
  Exec e{1700000003ULL, 42, 100, 2, 10000, 555};
  auto line = hft::test::encode_fix(e, 10);
  FixParser p;
  auto r = p.parse(as_bytes(line));
  const auto &g = get_event<Exec>(r);
  EXPECT_EQ(g.exec_qty, 2);
  EXPECT_EQ(g.px, 10000);
  EXPECT_EQ(g.trade_id, 555u);
}

TEST(FixParser, ReplaceRoundTrip) {
  Replace e{1700000004ULL, 42, 100, 101, Side::Sell, 10100, 4};
  auto line = hft::test::encode_fix(e, 11);
  FixParser p;
  auto r = p.parse(as_bytes(line));
  const auto &g = get_event<Replace>(r);
  EXPECT_EQ(g.old_id, 100u);
  EXPECT_EQ(g.new_id, 101u);
  EXPECT_EQ(g.side, Side::Sell);
}

TEST(FixParser, TradeRoundTrip) {
  Trade e{1700000005ULL, 42, 9999, 7, 777};
  auto line = hft::test::encode_fix(e, 12);
  FixParser p;
  auto r = p.parse(as_bytes(line));
  const auto &g = get_event<Trade>(r);
  EXPECT_EQ(g.px, 9999);
  EXPECT_EQ(g.qty, 7);
  EXPECT_EQ(g.trade_id, 777u);
}

TEST(FixParser, ClearRoundTrip) {
  Clear e{1700000006ULL, 42};
  auto line = hft::test::encode_fix(e, 13);
  FixParser p;
  auto r = p.parse(as_bytes(line));
  const auto &g = get_event<Clear>(r);
  EXPECT_EQ(g.ts, e.ts);
  EXPECT_EQ(g.sym, e.sym);
}

// ---------- Framing ----------------------------------------------------------

TEST(FixParser, NeedMoreOnEmptyBuffer) {
  FixParser p;
  EXPECT_EQ(p.parse({}).status, ParseResult::NeedMore);
}

TEST(FixParser, NeedMoreWithoutNewline) {
  FixParser p;
  std::string s = "35=C|34=1|52=1|55=42"; // no '\n'
  auto r = p.parse(as_bytes(s));
  EXPECT_EQ(r.status, ParseResult::NeedMore);
  EXPECT_EQ(r.consumed, 0u);
}

TEST(FixParser, MultipleMessagesSequential) {
  std::string buf;
  buf += hft::test::encode_fix(Cancel{1, 42, 100}, 1);
  buf += hft::test::encode_fix(Cancel{2, 42, 101}, 2);
  buf += hft::test::encode_fix(Clear{3, 42}, 3);

  FixParser p;
  std::span<const std::byte> view = as_bytes(buf);
  std::uint64_t seqs[3]{};
  for (int i = 0; i < 3; ++i) {
    auto r = p.parse(view);
    ASSERT_EQ(r.status, ParseResult::Ok) << "iteration " << i;
    seqs[i] = r.seq;
    view = view.subspan(r.consumed);
  }
  EXPECT_EQ(seqs[0], 1u);
  EXPECT_EQ(seqs[1], 2u);
  EXPECT_EQ(seqs[2], 3u);
  EXPECT_EQ(view.size(), 0u);
}

TEST(FixParser, PartialBufferReassembly) {
  std::string full = hft::test::encode_fix(
      Add{1700000000ULL, 42, 100, Side::Buy, 10000, 5}, 1);
  FixParser p;
  // First half: no newline yet.
  auto r1 = p.parse(as_bytes(std::string_view(full.data(), full.size() / 2)));
  EXPECT_EQ(r1.status, ParseResult::NeedMore);
  // Full buffer: parses cleanly.
  auto r2 = p.parse(as_bytes(full));
  EXPECT_EQ(r2.status, ParseResult::Ok);
  EXPECT_EQ(r2.consumed, full.size());
}

// ---------- Robustness -------------------------------------------------------

TEST(FixParser, UnknownTagIsIgnored) {
  // tag 9999 inserted in the middle.
  std::string line =
      "35=A|34=1|52=1|9999=garbage|55=42|37=100|54=1|44=10|38=1\n";
  FixParser p;
  auto r = p.parse(as_bytes(line));
  ASSERT_EQ(r.status, ParseResult::Ok);
  const auto &a = get_event<Add>(r);
  EXPECT_EQ(a.id, 100u);
  EXPECT_EQ(a.qty, 1);
}

TEST(FixParser, TagOrderIndependent) {
  // Same Add but tags shuffled.
  std::string line = "38=5|44=10000|54=1|37=100|55=42|52=1|34=1|35=A\n";
  FixParser p;
  auto r = p.parse(as_bytes(line));
  ASSERT_EQ(r.status, ParseResult::Ok);
  const auto &a = get_event<Add>(r);
  EXPECT_EQ(a.qty, 5);
  EXPECT_EQ(a.px, 10000);
}

TEST(FixParser, UnknownMsgTypeIsError) {
  std::string line = "35=Z|34=1|52=1|55=42\n";
  FixParser p;
  auto r = p.parse(as_bytes(line));
  EXPECT_EQ(r.status, ParseResult::Error);
  EXPECT_EQ(r.consumed, line.size()); // resync past newline
}

TEST(FixParser, MissingRequiredFieldIsError) {
  // Add without 38 (qty).
  std::string line = "35=A|34=1|52=1|55=42|37=100|54=1|44=10\n";
  FixParser p;
  auto r = p.parse(as_bytes(line));
  EXPECT_EQ(r.status, ParseResult::Error);
  EXPECT_EQ(r.consumed, line.size());
}

TEST(FixParser, MissingCommonFieldIsError) {
  // No SeqNum (34).
  std::string line = "35=C|52=1|55=42\n";
  FixParser p;
  auto r = p.parse(as_bytes(line));
  EXPECT_EQ(r.status, ParseResult::Error);
}

TEST(FixParser, BadIntegerIsError) {
  std::string line = "35=C|34=1|52=notanumber|55=42\n";
  FixParser p;
  auto r = p.parse(as_bytes(line));
  EXPECT_EQ(r.status, ParseResult::Error);
  EXPECT_EQ(r.consumed, line.size());
}

TEST(FixParser, BadSideIsError) {
  std::string line = "35=A|34=1|52=1|55=42|37=100|54=9|44=10|38=1\n";
  FixParser p;
  auto r = p.parse(as_bytes(line));
  EXPECT_EQ(r.status, ParseResult::Error);
}

TEST(FixParser, EmptyLineIsError) {
  std::string line = "\n";
  FixParser p;
  auto r = p.parse(as_bytes(line));
  EXPECT_EQ(r.status, ParseResult::Error);
  EXPECT_EQ(r.consumed, 1u);
}

TEST(FixParser, TrailingPipeIsError) {
  std::string line = "35=C|34=1|52=1|55=42|\n";
  FixParser p;
  auto r = p.parse(as_bytes(line));
  EXPECT_EQ(r.status, ParseResult::Error);
}

TEST(FixParser, DuplicateTagIsError) {
  std::string line = "35=C|34=1|34=2|52=1|55=42\n";
  FixParser p;
  auto r = p.parse(as_bytes(line));
  EXPECT_EQ(r.status, ParseResult::Error);
}

TEST(FixParser, ResyncsAfterBadFrame) {
  std::string buf;
  buf += "35=Z|34=1|52=1|55=42\n";               // bad: unknown msg type
  buf += hft::test::encode_fix(Clear{2, 42}, 2); // good

  FixParser p;
  std::span<const std::byte> view = as_bytes(buf);

  auto r1 = p.parse(view);
  EXPECT_EQ(r1.status, ParseResult::Error);
  view = view.subspan(r1.consumed);

  auto r2 = p.parse(view);
  ASSERT_EQ(r2.status, ParseResult::Ok);
  EXPECT_EQ(r2.seq, 2u);
  EXPECT_TRUE(std::holds_alternative<Clear>(*r2.event));
}