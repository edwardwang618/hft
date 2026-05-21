// include/hft/oe/oe_encoder.hpp
// Encodes strategy order requests into wire bytes.
// 将策略订单请求编码为线路字节。
#pragma once

#include <hft/core/types.hpp>
#include <hft/md/md_event.hpp>
#include <hft/oe/wire_oe.hpp>

#include <cstring>
#include <span>

namespace hft::oe {

// Stateless encoder: each method writes exactly one message into `out` and
// returns the number of bytes written (0 if buffer too small).
// 无状态编码器：每个方法将一条消息写入 out，返回写入字节数（缓冲不足时返回
// 0）。
struct OeEncoder {

  static std::size_t encode_new(std::span<std::byte> out, OrderId clord_id,
                                md::SymbolId sym, Side side, Price px,
                                Qty qty) noexcept {
    if (out.size() < sizeof(wire::NewOrder))
      return 0;
    wire::NewOrder msg{};
    msg.hdr.msg_type = wire::kNewOrder;
    msg.hdr.msg_len = sizeof(wire::NewOrder);
    msg.hdr.clord_id = clord_id;
    msg.hdr.sym = sym;
    msg.side = static_cast<uint8_t>(side);
    msg.px = px;
    msg.qty = qty;
    std::memcpy(out.data(), &msg, sizeof(msg));
    return sizeof(msg);
  }

  static std::size_t encode_cancel(std::span<std::byte> out, OrderId clord_id,
                                   md::SymbolId sym, OrderId exch_id) noexcept {
    if (out.size() < sizeof(wire::Cancel))
      return 0;
    wire::Cancel msg{};
    msg.hdr.msg_type = wire::kCancel;
    msg.hdr.msg_len = sizeof(wire::Cancel);
    msg.hdr.clord_id = clord_id;
    msg.hdr.sym = sym;
    msg.exch_id = exch_id;
    std::memcpy(out.data(), &msg, sizeof(msg));
    return sizeof(msg);
  }

  static std::size_t encode_replace(std::span<std::byte> out, OrderId clord_id,
                                    md::SymbolId sym, OrderId exch_id,
                                    Price new_px, Qty new_qty) noexcept {
    if (out.size() < sizeof(wire::Replace))
      return 0;
    wire::Replace msg{};
    msg.hdr.msg_type = wire::kReplace;
    msg.hdr.msg_len = sizeof(wire::Replace);
    msg.hdr.clord_id = clord_id;
    msg.hdr.sym = sym;
    msg.exch_id = exch_id;
    msg.px = new_px;
    msg.qty = new_qty;
    std::memcpy(out.data(), &msg, sizeof(msg));
    return sizeof(msg);
  }

  // Maximum possible frame size for buffer sizing.
  // 最大帧长，用于缓冲区预分配。
  static constexpr std::size_t kMaxFrameSize =
      sizeof(wire::Replace); // largest client→exchange message
};

} // namespace hft::oe
