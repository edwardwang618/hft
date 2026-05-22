// include/hft/exchange/in_process_session.hpp
// In-process OE session for integration testing: no real TCP socket.
// 用于集成测试的进程内 OE 会话：无真实 TCP socket。
//
// send_all() routes bytes directly to the exchange command sink (ExchangeSimulator).
// recv() always returns 0 — reports arrive via OrderGatewayT::report_sink(), not
// through poll_reports().
//
// send_all() 将字节直接路由到交易所命令 Sink（ExchangeSimulator）。
// recv() 始终返回 0——回报通过 OrderGatewayT::report_sink() 到达，而非 poll_reports()。
#pragma once

#include <hft/exchange/byte_sink.hpp>

#include <cstddef>
#include <sys/types.h> // ssize_t

namespace hft::exchange {

class InProcessSession {
public:
  explicit InProcessSession(IByteSink &exchange_cmd_sink) noexcept
      : sink_(exchange_cmd_sink) {}

  // Routes encoded OE bytes to the exchange simulator synchronously.
  // 将编码好的 OE 字节同步路由到交易所模拟器。
  bool send_all(const std::byte *data, std::size_t len) noexcept {
    sink_.on_bytes(data, len);
    return true;
  }

  // Reports come via report_sink(), not here. Always "nothing to read."
  // 回报通过 report_sink() 到达，此处无需读取，始终返回 0。
  ssize_t recv(std::byte *, std::size_t) noexcept { return 0; }

  bool connected() const noexcept { return true; }

private:
  IByteSink &sink_;
};

} // namespace hft::exchange
