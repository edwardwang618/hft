#pragma once
#include <cstdint>
#include <hft/core/types.hpp> // OrderId, Price, Qty, Ts, Side
#include <variant>

namespace hft::md {

using SymbolId = uint32_t;
using TradeId = uint64_t;

struct Add { // 新挂单
  Ts ts;
  SymbolId sym;
  OrderId id;
  Side side;
  Price px;
  Qty qty;
};

struct Cancel { // 整单撤销
  Ts ts;
  SymbolId sym;
  OrderId id;
};

struct Reduce { // 部分撤销: 剩余量变成 new_qty
  Ts ts;
  SymbolId sym;
  OrderId id;
  Qty new_qty;
};

struct Exec { // 挂单被成交 (maker 视角)
  Ts ts;
  SymbolId sym;
  OrderId id;
  Qty exec_qty;
  Price px;
  TradeId trade_id;
};

struct Replace { // 改单 = 旧 id 撤 + 新 id 挂, 队列位置丢失
  Ts ts;
  SymbolId sym;
  OrderId old_id;
  OrderId new_id;
  Side side;
  Price px;
  Qty qty;
};

struct Trade { // 非 book 成交 (隐藏单 / 集合竞价), 仅打印
  Ts ts;
  SymbolId sym;
  Price px;
  Qty qty;
  TradeId trade_id;
};

struct Clear { // 清空指定 symbol (session reset)
  Ts ts;
  SymbolId sym;
};

using MdEvent = std::variant<Add, Cancel, Reduce, Exec, Replace, Trade, Clear>;

} // namespace hft::md