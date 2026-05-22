// include/hft/exchange/fix_serializer.hpp
// Encodes MdEvents into the FIX-like ASCII wire format defined in wire_fix.hpp.
// 将 MdEvent 编码为 wire_fix.hpp 定义的类 FIX ASCII 线路格式。
#pragma once

#include <hft/exchange/wire_serializer.hpp>
#include <hft/md/md_event.hpp>
#include <hft/md/wire_fix.hpp>

#include <charconv>
#include <cstddef>
#include <cstdint>

namespace hft::exchange {

class FixSerializer final : public IWireSerializer {
public:
  std::size_t max_frame_size() const noexcept override { return 256; }

  std::size_t encode(const md::MdEvent &ev, std::uint64_t seq,
                     std::byte *out, std::size_t cap) noexcept override {
    return std::visit(
        [&](const auto &e) { return encode_one(e, seq, out, cap); }, ev);
  }

private:
  // Lightweight writer into a caller-owned char buffer.
  // 向调用方提供的 char 缓冲区写入数据的轻量写入器。
  struct W {
    char *p;
    const char *end;
    bool ok{true};

    void put_char(char c) {
      if (p < end)
        *p++ = c;
      else
        ok = false;
    }

    // Writes "tag=" (integer tag followed by '=').
    // 写入 "tag="（整数 tag 后跟 '='）。
    void put_tag(int tag) {
      char tmp[8];
      auto [ptr, ec] = std::to_chars(tmp, tmp + sizeof(tmp), tag);
      if (ec != std::errc{}) {
        ok = false;
        return;
      }
      for (char *t = tmp; t < ptr; ++t)
        put_char(*t);
      put_char('=');
    }

    template <class T> void put_int(T v) {
      char tmp[24];
      auto [ptr, ec] = std::to_chars(tmp, tmp + sizeof(tmp), v);
      if (ec != std::errc{}) {
        ok = false;
        return;
      }
      for (char *t = tmp; t < ptr; ++t)
        put_char(*t);
    }

    void sep() { put_char(md::wire_fix::kFieldSep); }
    void end_frame() { put_char(md::wire_fix::kFrameEnd); }

    std::size_t size(const char *start) const noexcept {
      return static_cast<std::size_t>(p - start);
    }
  };

  // Common header fields: 35, 34, 52, 55. Does NOT add a trailing separator
  // after sym — body fields must add one before their first field if needed.
  // 公共头部字段：35、34、52、55。sym 后不加尾部分隔符，由各消息体字段
  // 按需在首个字段前添加。
  static void write_header(W &w, char msg_type, uint64_t seq,
                            uint64_t ts, uint32_t sym) noexcept {
    using namespace md::wire_fix;
    w.put_tag(kTagMsgType); w.put_char(msg_type); w.sep();
    w.put_tag(kTagSeqNum);  w.put_int(seq);        w.sep();
    w.put_tag(kTagTs);      w.put_int(ts);          w.sep();
    w.put_tag(kTagSymbol);  w.put_int(sym);
    // No trailing sep — caller adds sep() before the first body field.
  }

  std::size_t encode_one(const md::Add &e, uint64_t seq,
                         std::byte *out, std::size_t cap) noexcept {
    char *buf = reinterpret_cast<char *>(out);
    W w{buf, buf + cap};
    using namespace md::wire_fix;
    write_header(w, kAdd, seq, e.ts, e.sym);
    w.sep(); w.put_tag(kTagOrderId); w.put_int(e.id);
    w.sep(); w.put_tag(kTagSide);
             w.put_int(e.side == Side::Buy ? kSideBuy : kSideSell);
    w.sep(); w.put_tag(kTagPrice);   w.put_int(e.px);
    w.sep(); w.put_tag(kTagQty);     w.put_int(e.qty);
    w.end_frame();
    return w.ok ? w.size(buf) : 0;
  }

  std::size_t encode_one(const md::Cancel &e, uint64_t seq,
                         std::byte *out, std::size_t cap) noexcept {
    char *buf = reinterpret_cast<char *>(out);
    W w{buf, buf + cap};
    using namespace md::wire_fix;
    write_header(w, kCancel, seq, e.ts, e.sym);
    w.sep(); w.put_tag(kTagOrderId); w.put_int(e.id);
    w.end_frame();
    return w.ok ? w.size(buf) : 0;
  }

  std::size_t encode_one(const md::Reduce &e, uint64_t seq,
                         std::byte *out, std::size_t cap) noexcept {
    char *buf = reinterpret_cast<char *>(out);
    W w{buf, buf + cap};
    using namespace md::wire_fix;
    write_header(w, kReduce, seq, e.ts, e.sym);
    w.sep(); w.put_tag(kTagOrderId); w.put_int(e.id);
    w.sep(); w.put_tag(kTagNewQty);  w.put_int(e.new_qty);
    w.end_frame();
    return w.ok ? w.size(buf) : 0;
  }

  std::size_t encode_one(const md::Exec &e, uint64_t seq,
                         std::byte *out, std::size_t cap) noexcept {
    char *buf = reinterpret_cast<char *>(out);
    W w{buf, buf + cap};
    using namespace md::wire_fix;
    write_header(w, kExec, seq, e.ts, e.sym);
    w.sep(); w.put_tag(kTagOrderId);  w.put_int(e.id);
    w.sep(); w.put_tag(kTagLastQty);  w.put_int(e.exec_qty);
    w.sep(); w.put_tag(kTagPrice);    w.put_int(e.px);
    w.sep(); w.put_tag(kTagTradeId);  w.put_int(e.trade_id);
    w.end_frame();
    return w.ok ? w.size(buf) : 0;
  }

  std::size_t encode_one(const md::Replace &e, uint64_t seq,
                         std::byte *out, std::size_t cap) noexcept {
    char *buf = reinterpret_cast<char *>(out);
    W w{buf, buf + cap};
    using namespace md::wire_fix;
    write_header(w, kReplace, seq, e.ts, e.sym);
    w.sep(); w.put_tag(kTagOrigOrderId); w.put_int(e.old_id);
    w.sep(); w.put_tag(kTagOrderId);     w.put_int(e.new_id);
    w.sep(); w.put_tag(kTagSide);
             w.put_int(e.side == Side::Buy ? kSideBuy : kSideSell);
    w.sep(); w.put_tag(kTagPrice); w.put_int(e.px);
    w.sep(); w.put_tag(kTagQty);   w.put_int(e.qty);
    w.end_frame();
    return w.ok ? w.size(buf) : 0;
  }

  std::size_t encode_one(const md::Trade &e, uint64_t seq,
                         std::byte *out, std::size_t cap) noexcept {
    char *buf = reinterpret_cast<char *>(out);
    W w{buf, buf + cap};
    using namespace md::wire_fix;
    write_header(w, kTrade, seq, e.ts, e.sym);
    w.sep(); w.put_tag(kTagPrice);   w.put_int(e.px);
    w.sep(); w.put_tag(kTagQty);     w.put_int(e.qty);
    w.sep(); w.put_tag(kTagTradeId); w.put_int(e.trade_id);
    w.end_frame();
    return w.ok ? w.size(buf) : 0;
  }

  std::size_t encode_one(const md::Clear &e, uint64_t seq,
                         std::byte *out, std::size_t cap) noexcept {
    char *buf = reinterpret_cast<char *>(out);
    W w{buf, buf + cap};
    write_header(w, md::wire_fix::kClear, seq, e.ts, e.sym);
    // Clear has no body fields; write_header already wrote sym without trailing sep.
    // Clear 无消息体字段；write_header 已写完 sym 且无尾部分隔符。
    w.end_frame();
    return w.ok ? w.size(buf) : 0;
  }
};

} // namespace hft::exchange
