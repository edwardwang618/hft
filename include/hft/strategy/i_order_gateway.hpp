#pragma once

#include <hft/core/types.hpp>
#include <hft/md/md_event.hpp>

namespace hft::strategy {

class IOrderGateway {
public:
  virtual ~IOrderGateway() = default;
  virtual void send_new(md::SymbolId sym, Side side, Price px, Qty qty) = 0;
  virtual void send_cancel(OrderId id) = 0;
  virtual void send_replace(OrderId old_id, Price new_px, Qty new_qty) = 0;
};

} // namespace hft::strategy
