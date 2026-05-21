// include/hft/oe/oe_decoder.hpp
// Parses execution report bytes from the exchange and fires IOrderListener
// callbacks. 解析交易所返回的回报字节，触发 IOrderListener 回调。
#pragma once

#include <hft/core/types.hpp>
#include <hft/oe/wire_oe.hpp>
#include <hft/strategy/i_order_listener.hpp>

#include <cstring>
#include <span>
#include <string_view>
#include <vector>

namespace hft::oe {

// Result of one parse attempt — mirrors the MD-side ParseResult pattern.
// 单次解析结果——与 MD 侧 ParseResult 模式对称。
struct OeParseResult {
  enum Status { Ok, NeedMore, Error } status{NeedMore};
  std::size_t consumed{0};
};

// Stateful stream decoder: accumulates raw bytes, extracts complete frames,
// dispatches to IOrderListener.
// 有状态流式解码器：积累原始字节，提取完整帧，分发到 IOrderListener。
//
// Usage:
//   OeDecoder dec(listener);
//   dec.on_bytes(buf, n);   // called each time TCP delivers bytes
//
class OeDecoder {
public:
  explicit OeDecoder(strategy::IOrderListener &listener)
      : listener_(listener) {}

  // Feed raw bytes from the TCP receive buffer.
  // Internally loops until NeedMore; fires listener callbacks for each frame.
  // 喂入 TCP 接收缓冲区的原始字节，循环解析直到 NeedMore，每帧触发监听器回调。
  void on_bytes(const std::byte *data, std::size_t n) {
    buf_.insert(buf_.end(), data, data + n);
    std::size_t offset = 0;
    while (true) {
      auto r = parse_one({buf_.data() + offset, buf_.size() - offset});
      if (r.status == OeParseResult::NeedMore)
        break;
      offset += r.consumed;
      if (r.status == OeParseResult::Error) {
        // Skip the bad header and try to re-sync.
        // 跳过损坏帧，尝试重新同步。
        if (offset == 0)
          ++offset; // avoid infinite loop
      }
    }
    if (offset > 0)
      buf_.erase(buf_.begin(),
                 buf_.begin() + static_cast<std::ptrdiff_t>(offset));
  }

private:
  strategy::IOrderListener &listener_;
  std::vector<std::byte> buf_;

  OeParseResult parse_one(std::span<const std::byte> view) {
    if (view.size() < sizeof(wire::Header))
      return {OeParseResult::NeedMore, 0};

    wire::Header hdr{};
    std::memcpy(&hdr, view.data(), sizeof(hdr));

    if (hdr.msg_len < sizeof(wire::Header) || hdr.msg_len > 256)
      return {OeParseResult::Error, sizeof(wire::Header)};

    if (view.size() < hdr.msg_len)
      return {OeParseResult::NeedMore, 0};

    dispatch(hdr, view.subspan(0, hdr.msg_len));
    return {OeParseResult::Ok, hdr.msg_len};
  }

  void dispatch(const wire::Header &hdr, std::span<const std::byte> frame) {
    switch (hdr.msg_type) {

    case wire::kAck: {
      if (frame.size() < sizeof(wire::Ack))
        break;
      wire::Ack msg{};
      std::memcpy(&msg, frame.data(), sizeof(msg));
      listener_.on_ack(hdr.clord_id, msg.exch_id);
      break;
    }

    case wire::kFill: {
      if (frame.size() < sizeof(wire::Fill))
        break;
      wire::Fill msg{};
      std::memcpy(&msg, frame.data(), sizeof(msg));
      listener_.on_fill(msg.exch_id, msg.filled_qty, msg.px);
      break;
    }

    case wire::kCancelAck: {
      if (frame.size() < sizeof(wire::CancelAck))
        break;
      wire::CancelAck msg{};
      std::memcpy(&msg, frame.data(), sizeof(msg));
      listener_.on_cancel_ack(msg.exch_id);
      break;
    }

    case wire::kReject: {
      if (frame.size() < sizeof(wire::Reject))
        break;
      wire::Reject msg{};
      std::memcpy(&msg, frame.data(), sizeof(msg));
      // Reason is null-padded; construct a safe string_view.
      // reason 以 null 填充；构造安全的 string_view。
      const std::string_view reason(
          msg.reason, ::strnlen(msg.reason, wire::kRejectReasonLen));
      listener_.on_reject(hdr.clord_id, reason);
      break;
    }

    default:
      break; // unknown report type, ignore
    }
  }
};

} // namespace hft::oe
