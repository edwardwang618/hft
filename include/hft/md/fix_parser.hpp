#pragma once

#include <charconv>
#include <hft/md/md_event.hpp>
#include <hft/md/parser.hpp>
#include <hft/md/wire_fix.hpp>
#include <optional>
#include <string_view>

namespace hft::md {

class FixParser final : public IParser {
public:
  ParseResult parse(std::span<const std::byte> buf) override {
    const char *data = reinterpret_cast<const char *>(buf.data());
    const std::size_t n = buf.size();

    // Find end-of-frame.
    std::size_t nl = 0;
    while (nl < n && data[nl] != wire_fix::kFrameEnd)
      ++nl;
    if (nl == n)
      return ParseResult::need_more();

    const std::size_t consumed = nl + 1;
    return parse_line(std::string_view(data, nl), consumed);
  }

private:
  // Slots for every tag we recognise. All optional => not present.
  struct Fields {
    std::optional<char> msg_type;     // 35
    std::optional<uint64_t> seq;      // 34
    std::optional<uint64_t> ts;       // 52
    std::optional<uint32_t> sym;      // 55
    std::optional<uint64_t> id;       // 37
    std::optional<uint64_t> orig_id;  // 41
    std::optional<int> side;          // 54
    std::optional<int64_t> px;        // 44
    std::optional<int64_t> qty;       // 38
    std::optional<int64_t> last_qty;  // 32
    std::optional<int64_t> new_qty;   // 39
    std::optional<uint64_t> trade_id; // 17
  };

  static ParseResult parse_line(std::string_view line, std::size_t consumed) {
    if (line.empty())
      return ParseResult::error(consumed);

    Fields f{};

    // Tokenize on '|', then split each token on the first '='.
    std::size_t i = 0;
    while (i <= line.size()) {
      std::size_t end = line.find(wire_fix::kFieldSep, i);
      if (end == std::string_view::npos)
        end = line.size();

      std::string_view field = line.substr(i, end - i);
      if (field.empty())
        return ParseResult::error(consumed); // empty token
      std::size_t eq = field.find('=');
      if (eq == 0 || eq == std::string_view::npos)
        return ParseResult::error(consumed);

      int tag{};
      if (!parse_int(field.substr(0, eq), tag) || tag < 0)
        return ParseResult::error(consumed);
      if (!assign_field(f, tag, field.substr(eq + 1)))
        return ParseResult::error(consumed);

      if (end == line.size())
        break;
      i = end + 1;
    }

    // Common required tags.
    if (!f.msg_type || !f.seq || !f.ts || !f.sym)
      return ParseResult::error(consumed);

    const std::uint64_t seq = *f.seq;

    auto ok = [&](MdEvent ev) {
      return ParseResult::ok(consumed, std::move(ev), seq);
    };

    switch (*f.msg_type) {
    case wire_fix::kAdd: {
      if (!f.id || !f.side || !f.px || !f.qty)
        return ParseResult::error(consumed);
      Side s;
      if (!side_from_int(*f.side, s))
        return ParseResult::error(consumed);
      return ok(Add{*f.ts, *f.sym, *f.id, s, *f.px, *f.qty});
    }
    case wire_fix::kCancel: {
      if (!f.id)
        return ParseResult::error(consumed);
      return ok(Cancel{*f.ts, *f.sym, *f.id});
    }
    case wire_fix::kReduce: {
      if (!f.id || !f.new_qty)
        return ParseResult::error(consumed);
      return ok(Reduce{*f.ts, *f.sym, *f.id, *f.new_qty});
    }
    case wire_fix::kExec: {
      if (!f.id || !f.last_qty || !f.px || !f.trade_id)
        return ParseResult::error(consumed);
      return ok(Exec{*f.ts, *f.sym, *f.id, *f.last_qty, *f.px, *f.trade_id});
    }
    case wire_fix::kReplace: {
      if (!f.orig_id || !f.id || !f.side || !f.px || !f.qty)
        return ParseResult::error(consumed);
      Side s;
      if (!side_from_int(*f.side, s))
        return ParseResult::error(consumed);
      return ok(Replace{*f.ts, *f.sym, *f.orig_id, *f.id, s, *f.px, *f.qty});
    }
    case wire_fix::kTrade: {
      if (!f.px || !f.qty || !f.trade_id)
        return ParseResult::error(consumed);
      return ok(Trade{*f.ts, *f.sym, *f.px, *f.qty, *f.trade_id});
    }
    case wire_fix::kClear:
      return ok(Clear{*f.ts, *f.sym});
    default:
      return ParseResult::error(consumed);
    }
  }

  static bool assign_field(Fields &f, int tag, std::string_view v) {
    using namespace wire_fix;
    switch (tag) {
    case kTagMsgType:
      if (v.size() != 1)
        return false;
      f.msg_type = v[0];
      return true;
    case kTagSeqNum:
      return parse_into(v, f.seq);
    case kTagTs:
      return parse_into(v, f.ts);
    case kTagSymbol:
      return parse_into(v, f.sym);
    case kTagOrderId:
      return parse_into(v, f.id);
    case kTagOrigOrderId:
      return parse_into(v, f.orig_id);
    case kTagSide:
      return parse_into(v, f.side);
    case kTagPrice:
      return parse_into(v, f.px);
    case kTagQty:
      return parse_into(v, f.qty);
    case kTagLastQty:
      return parse_into(v, f.last_qty);
    case kTagNewQty:
      return parse_into(v, f.new_qty);
    case kTagTradeId:
      return parse_into(v, f.trade_id);
    default:
      return true; // unknown tag: ignore (option a)
    }
  }

  template <class T> static bool parse_int(std::string_view s, T &out) {
    if (s.empty())
      return false;
    auto *first = s.data();
    auto *last = s.data() + s.size();
    auto [p, ec] = std::from_chars(first, last, out);
    return ec == std::errc{} && p == last;
  }

  template <class T>
  static bool parse_into(std::string_view s, std::optional<T> &slot) {
    if (slot.has_value())
      return false; // duplicate tag
    T tmp{};
    if (!parse_int(s, tmp))
      return false;
    slot = tmp;
    return true;
  }

  static bool side_from_int(int v, Side &out) {
    if (v == wire_fix::kSideBuy) {
      out = Side::Buy;
      return true;
    }
    if (v == wire_fix::kSideSell) {
      out = Side::Sell;
      return true;
    }
    return false;
  }
};

} // namespace hft::md