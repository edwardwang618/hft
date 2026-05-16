// include/hft/strategy/i_order_gateway.hpp
// Abstract order gateway interface; intentionally virtual (off hot-path).
// 抽象订单网关接口；有意使用虚函数（不在热路径上）。
#pragma once

#include <hft/core/types.hpp>
#include <hft/md/md_event.hpp>

namespace hft::strategy {

// Strategies call these methods to send orders; the concrete implementation
// connects to the exchange (TCP, SHM, etc.).
// 策略通过这些方法发送订单；具体实现负责连接交易所（TCP、共享内存等）。
class IOrderGateway {
public:
  virtual ~IOrderGateway() = default;

  // Send a new limit order; returns the assigned client order id.
  // 发送新的限价单；返回分配的客户端订单 ID。
  virtual OrderId send_new(md::SymbolId sym, Side side, Price px, Qty qty) = 0;

  // Cancel an existing order by id. 按 ID 撤销已有订单。
  virtual void send_cancel(OrderId id) = 0;

  // Amend price and/or qty of an existing order. 修改已有订单的价格和/或数量。
  virtual void send_replace(OrderId old_id, Price new_px, Qty new_qty) = 0;
};

} // namespace hft::strategy
