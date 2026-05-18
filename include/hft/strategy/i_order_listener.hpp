// include/hft/strategy/i_order_listener.hpp
// Callbacks from the exchange back into the OrderManager.
// Exchange 回报注入 OrderManager 的接口。
#pragma once

#include <hft/core/types.hpp>
#include <string_view>

namespace hft::strategy {

// Implemented by OrderManager; called by the exchange gateway when
// execution reports arrive (ack, fill, cancel confirm, reject).
// 由 OrderManager 实现；exchange 网关收到回报时调用。
class IOrderListener {
public:
  virtual ~IOrderListener() = default;

  // New-order accepted by exchange; exch_id is the exchange-assigned id.
  // 交易所确认新单；exch_id 是交易所分配的订单号。
  virtual void on_ack(OrderId clord_id, OrderId exch_id) = 0;

  // Partial or full fill. filled_qty is the qty of THIS fill, not cumulative.
  // 部分或全部成交。filled_qty 是本次成交量，非累计。
  virtual void on_fill(OrderId exch_id, Qty filled_qty, Price px) = 0;

  // Cancel confirmed by exchange.
  // 交易所确认撤单。
  virtual void on_cancel_ack(OrderId exch_id) = 0;

  // New-order rejected. Exchange has not assigned an exch_id yet, so we
  // identify the order by our own clord_id.
  // 新单被拒。交易所尚未分配 exch_id，因此用我们的 clord_id 标识。
  virtual void on_reject(OrderId clord_id, std::string_view reason) = 0;
};

} // namespace hft::strategy
