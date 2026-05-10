#include <cstddef>
#include <cstdint>
#include <cstring>
#include <gtest/gtest.h>
#include <hft/md/binary_parser.hpp>
#include <hft/md/wire_binary.hpp>
#include <span>
#include <vector>

using namespace hft;
using namespace hft::md;

namespace {

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
  std::span<const std::byte> span() const { return {data.data(), data.size()}; }
};

Buf enc_header(uint8_t type) {
  Buf b;
  b.put<uint8_t>(type);
  b.put<uint8_t>(0);
  b.put<uint16_t>(0); // placeholder, patch later
  return b;
}

} // namespace

TEST(BinaryParser, NeedMoreOnEmpty) {
  BinaryParser p;
  auto r = p.parse({});
  EXPECT_EQ(r.status, ParseResult::NeedMore);
}

TEST(BinaryParser, NeedMoreOnPartialHeader) {
  BinaryParser p;
  std::byte b[2]{};
  auto r = p.parse(std::span<const std::byte>(b, 2));
  EXPECT_EQ(r.status, ParseResult::NeedMore);
}

TEST(BinaryParser, Add_Roundtrip) {
  auto b = enc_header(wire::kAdd);
  b.put<uint64_t>(1000);  // ts
  b.put<uint32_t>(42);   // sym
  b.put<uint64_t>(7);    // id
  b.put<uint8_t>(0);     // side = Buy
  b.put<int64_t>(12345); // px
  b.put<int64_t>(10);    // qty
  b.patch_len();

  BinaryParser p;
  auto r = p.parse(b.span());
  ASSERT_EQ(r.status, ParseResult::Ok);
  EXPECT_EQ(r.consumed, b.size());
  ASSERT_TRUE(r.event.has_value());
  ASSERT_TRUE(std::holds_alternative<Add>(*r.event));
  auto &a = std::get<Add>(*r.event);
  EXPECT_EQ(a.ts, 1000);
  EXPECT_EQ(a.sym, 42u);
  EXPECT_EQ(a.id, 7u);
  EXPECT_EQ(a.side, Side::Buy);
  EXPECT_EQ(a.px, 12345);
  EXPECT_EQ(a.qty, 10);
}

TEST(BinaryParser, NeedMoreWhenBodyTruncated) {
  auto b = enc_header(wire::kAdd);
  b.put<uint64_t>(1000);
  b.put<uint32_t>(42);
  b.patch_len();
  // 故意把 msg_len 写成"满", 但只喂前一半
  uint16_t full = static_cast<uint16_t>(b.size() + 100);
  std::memcpy(b.data.data() + 2, &full, sizeof(full));

  BinaryParser p;
  auto r = p.parse(b.span());
  EXPECT_EQ(r.status, ParseResult::NeedMore);
}

TEST(BinaryParser, UnknownMsgTypeIsError) {
  auto b = enc_header(99);
  b.patch_len();
  BinaryParser p;
  auto r = p.parse(b.span());
  EXPECT_EQ(r.status, ParseResult::Error);
}

TEST(BinaryParser, Exec_Roundtrip) {
  auto b = enc_header(wire::kExec);
  b.put<int64_t>(2000);
  b.put<uint32_t>(42);
  b.put<uint64_t>(7);
  b.put<int64_t>(3);     // exec_qty
  b.put<int64_t>(12345); // px
  b.put<uint64_t>(999);  // trade_id
  b.patch_len();

  BinaryParser p;
  auto r = p.parse(b.span());
  ASSERT_EQ(r.status, ParseResult::Ok);
  auto &e = std::get<Exec>(*r.event);
  EXPECT_EQ(e.trade_id, 999u);
  EXPECT_EQ(e.exec_qty, 3);
}

TEST(BinaryParser, TwoMessagesInOneBuffer) {
  auto b1 = enc_header(wire::kCancel);
  b1.put<uint64_t>(1);
  b1.put<uint32_t>(1);
  b1.put<uint64_t>(1);
  b1.patch_len();

  auto b2 = enc_header(wire::kClear);
  b2.put<uint64_t>(2);
  b2.put<uint32_t>(1);
  b2.patch_len();

  std::vector<std::byte> all;
  all.insert(all.end(), b1.data.begin(), b1.data.end());
  all.insert(all.end(), b2.data.begin(), b2.data.end());

  BinaryParser p;
  auto r1 = p.parse({all.data(), all.size()});
  ASSERT_EQ(r1.status, ParseResult::Ok);
  EXPECT_TRUE(std::holds_alternative<Cancel>(*r1.event));

  auto r2 = p.parse({all.data() + r1.consumed, all.size() - r1.consumed});
  ASSERT_EQ(r2.status, ParseResult::Ok);
  EXPECT_TRUE(std::holds_alternative<Clear>(*r2.event));
}