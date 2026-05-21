// include/hft/oe/order_gateway.hpp
// Concrete IOrderGateway: encodes orders → TCP → exchange.
// Also drives OeDecoder from incoming TCP bytes.
// 具体的 IOrderGateway 实现：将订单编码后经 TCP 发往交易所。
// 同时驱动 OeDecoder 处理收到的 TCP 回报字节。
//
// Fits into the two-thread model:
//   Hot-path thread: calls send_new / send_cancel (encodes + send_all)
//   Same or separate poller thread: calls poll_reports() in a loop
//
// 适配两线程模型：
//   热路径线程：调用 send_new / send_cancel（编码 + send_all）
//   同一线程或独立轮询线程：循环调用 poll_reports()
#pragma once

#include <hft/core/types.hpp>
#include <hft/md/md_event.hpp>
#include <hft/oe/oe_decoder.hpp>
#include <hft/oe/oe_encoder.hpp>
#include <hft/oe/tcp_session.hpp>
#include <hft/strategy/i_order_gateway.hpp>
#include <hft/strategy/i_order_listener.hpp>

#include <array>
#include <atomic>
#include <cstdint>

namespace hft::oe {

class OrderGateway final : public strategy::IOrderGateway {
public:
  // listener: receives on_ack / on_fill / on_cancel_ack / on_reject
  //           (typically OrderManager)
  // session:  caller owns the TcpSession; call session.connect() before use.
  // 监听器接收 ACK/Fill/CancelAck/Reject 回调（通常是 OrderManager）。
  // 调用方持有 TcpSession；使用前先调用 session.connect()。
  OrderGateway(strategy::IOrderListener &listener, TcpSession &session)
      : decoder_(listener), session_(session) {}

  // ── IOrderGateway ─────────────────────────────────────────────────

  OrderId send_new(md::SymbolId sym, Side side, Price px, Qty qty) override {
    const OrderId id = next_id_.fetch_add(1, std::memory_order_relaxed);
    const std::size_t n = OeEncoder::encode_new({buf_.data(), buf_.size()}, id,
                                                sym, side, px, qty);
    session_.send_all(buf_.data(), n);
    return id;
  }

  void send_cancel(OrderId clord_id) override {
    // exch_id not tracked here — caller (OrderManager) must provide it.
    // This overload is a no-op placeholder; use send_cancel_with_exch_id.
    // exch_id 在此不追踪——调用方（OrderManager）需提供。
    // 此重载为占位符；请使用 send_cancel_with_exch_id。
    (void)clord_id;
  }

  // Extended cancel that includes exch_id (called by OrderManager).
  // 包含 exch_id 的扩展撤单接口（由 OrderManager 调用）。
  void send_cancel(OrderId clord_id, md::SymbolId sym, OrderId exch_id) {
    const std::size_t n = OeEncoder::encode_cancel({buf_.data(), buf_.size()},
                                                   clord_id, sym, exch_id);
    session_.send_all(buf_.data(), n);
  }

  void send_replace(OrderId clord_id, Price new_px, Qty new_qty) override {
    (void)clord_id;
    (void)new_px;
    (void)new_qty;
    // Requires exch_id; full implementation needs OrderManager cooperation.
    // 需要 exch_id；完整实现需要 OrderManager 配合。
  }

  // ── Report poller ─────────────────────────────────────────────────
  // Call in a tight loop on the poller thread (or hot-path thread).
  // Reads up to one recv-buffer worth of bytes and drives OeDecoder.
  // 在轮询线程（或热路径线程）上紧密循环调用。
  // 最多读取一个接收缓冲区的字节并驱动 OeDecoder。
  void poll_reports() noexcept {
    std::array<std::byte, 4096> rbuf;
    const ssize_t n = session_.recv(rbuf.data(), rbuf.size());
    if (n > 0)
      decoder_.on_bytes(rbuf.data(), static_cast<std::size_t>(n));
  }

  bool connected() const noexcept { return session_.connected(); }

private:
  OeDecoder decoder_;
  TcpSession &session_;
  std::atomic<OrderId> next_id_{1};
  std::array<std::byte, OeEncoder::kMaxFrameSize> buf_{};
};

} // namespace hft::oe
