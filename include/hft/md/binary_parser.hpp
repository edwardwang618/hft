#pragma once

#include <cstring>
#include <hft/md/md_event.hpp>
#include <hft/md/parser.hpp>
#include <hft/md/wire_binary.hpp>
#include <type_traits>

namespace hft::md {

static_assert(sizeof(Ts) == 8 && std::is_unsigned_v<Ts>);
static_assert(sizeof(OrderId) == 8 && std::is_unsigned_v<OrderId>);
static_assert(sizeof(Price) == 8 && std::is_signed_v<Price>);
static_assert(sizeof(Qty) == 8 && std::is_signed_v<Qty>);
static_assert(sizeof(SymbolId) == 4);
static_assert(sizeof(TradeId) == 8);

class BinaryParser final : public IParser {
public:
  ParseResult parse(std::span<const std::byte> buf) override {
    if (buf.size() < sizeof(wire::Header))
      return ParseResult::need_more();

    wire::Header h{};
    std::memcpy(&h, buf.data(), sizeof(h));

    // header sanity: 在相信 msg_len 之前就要判掉不可能的值
    constexpr std::size_t kMaxFrameLen = 4096; // 按你协议调
    if (h.msg_len < sizeof(wire::Header) || h.msg_len > kMaxFrameLen) {
      // msg_len 不可信, 没法定位下一帧. 把当前 buffer 全扔掉.
      return ParseResult::error(buf.size());
    }

    // 可选: msg_type 也在这里白名单过一遍, 不认识就当整帧坏
    // (也可以留到 default 分支, 反正那时 msg_len 已经可信了)

    if (buf.size() < h.msg_len)
      return ParseResult::need_more();

    const std::byte *p = buf.data() + sizeof(h);
    const size_t body_len = h.msg_len - sizeof(h);
    std::span<const std::byte> body(p, body_len);

    switch (h.msg_type) {
    case wire::kAdd:
      return decode_add(h, body);
    case wire::kCancel:
      return decode_cancel(h, body);
    case wire::kReduce:
      return decode_reduce(h, body);
    case wire::kExec:
      return decode_exec(h, body);
    case wire::kReplace:
      return decode_replace(h, body);
    case wire::kTrade:
      return decode_trade(h, body);
    case wire::kClear:
      return decode_clear(h, body);
    default:
      return ParseResult::error(h.msg_len);
    }
  }

private:
  // 从 body 按顺序读固定长度字段
  template <class T> static bool read(std::span<const std::byte> &s, T &out) {
    if (s.size() < sizeof(T))
      return false;
    std::memcpy(&out, s.data(), sizeof(T));
    s = s.subspan(sizeof(T));
    return true;
  }

  static ParseResult decode_add(const wire::Header &h,
                                std::span<const std::byte> b) {
    Add ev{};
    uint8_t side_raw{};
    if (!read(b, ev.ts) || !read(b, ev.sym) || !read(b, ev.id) ||
        !read(b, side_raw) || !read(b, ev.px) || !read(b, ev.qty) || !b.empty())
      return ParseResult::error(h.msg_len);
    ev.side = static_cast<Side>(side_raw);
    return ParseResult::ok(h.msg_len, ev, h.seq);
  }

  static ParseResult decode_cancel(const wire::Header &h,
                                   std::span<const std::byte> b) {
    Cancel ev{};
    if (!read(b, ev.ts) || !read(b, ev.sym) || !read(b, ev.id) || !b.empty())
      return ParseResult::error(h.msg_len);
    return ParseResult::ok(h.msg_len, ev, h.seq);
  }

  static ParseResult decode_reduce(const wire::Header &h,
                                   std::span<const std::byte> b) {
    Reduce ev{};
    if (!read(b, ev.ts) || !read(b, ev.sym) || !read(b, ev.id) ||
        !read(b, ev.new_qty) || !b.empty())
      return ParseResult::error(h.msg_len);
    return ParseResult::ok(h.msg_len, ev, h.seq);
  }

  static ParseResult decode_exec(const wire::Header &h,
                                 std::span<const std::byte> b) {
    Exec ev{};
    if (!read(b, ev.ts) || !read(b, ev.sym) || !read(b, ev.id) ||
        !read(b, ev.exec_qty) || !read(b, ev.px) || !read(b, ev.trade_id) ||
        !b.empty())
      return ParseResult::error(h.msg_len);
    return ParseResult::ok(h.msg_len, ev, h.seq);
  }

  static ParseResult decode_replace(const wire::Header &h,
                                    std::span<const std::byte> b) {
    Replace ev{};
    uint8_t side_raw{};
    if (!read(b, ev.ts) || !read(b, ev.sym) || !read(b, ev.old_id) ||
        !read(b, ev.new_id) || !read(b, side_raw) || !read(b, ev.px) ||
        !read(b, ev.qty) || !b.empty())
      return ParseResult::error(h.msg_len);
    ev.side = static_cast<Side>(side_raw);
    return ParseResult::ok(h.msg_len, ev, h.seq);
  }

  static ParseResult decode_trade(const wire::Header &h,
                                  std::span<const std::byte> b) {
    Trade ev{};
    if (!read(b, ev.ts) || !read(b, ev.sym) || !read(b, ev.px) ||
        !read(b, ev.qty) || !read(b, ev.trade_id) || !b.empty())
      return ParseResult::error(h.msg_len);
    return ParseResult::ok(h.msg_len, ev, h.seq);
  }

  static ParseResult decode_clear(const wire::Header &h,
                                  std::span<const std::byte> b) {
    Clear ev{};
    if (!read(b, ev.ts) || !read(b, ev.sym) || !b.empty())
      return ParseResult::error(h.msg_len);
    return ParseResult::ok(h.msg_len, ev, h.seq);
  }
};

} // namespace hft::md