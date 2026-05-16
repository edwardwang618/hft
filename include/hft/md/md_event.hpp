// include/hft/md/md_event.hpp
// Market-data event types and the MdEvent variant.
// 行情事件类型定义及 MdEvent 变体。
#pragma once

#include <cstdint>
#include <hft/core/types.hpp> // OrderId, Price, Qty, Ts, Side
#include <variant>

namespace hft::md {

using SymbolId = uint32_t;
using TradeId  = uint64_t;

struct Add {     // New resting order / 新挂单
  Ts       ts;
  SymbolId sym;
  OrderId  id;
  Side     side;
  Price    px;
  Qty      qty;
};

struct Cancel {  // Full cancel / 整单撤销
  Ts       ts;
  SymbolId sym;
  OrderId  id;
};

struct Reduce {  // Partial cancel: residual qty becomes new_qty / 部分撤销：剩余量变为 new_qty
  Ts       ts;
  SymbolId sym;
  OrderId  id;
  Qty      new_qty;
};

struct Exec {    // Passive fill (maker side) / 挂单被成交（maker 视角）
  Ts       ts;
  SymbolId sym;
  OrderId  id;
  Qty      exec_qty;
  Price    px;
  TradeId  trade_id;
};

struct Replace { // Amend = cancel old id + add new id; loses queue priority
                 // 改单 = 旧 id 撤 + 新 id 挂，队列优先级丢失
  Ts       ts;
  SymbolId sym;
  OrderId  old_id;
  OrderId  new_id;
  Side     side;
  Price    px;
  Qty      qty;
};

struct Trade {   // Off-book print (hidden order / auction); no book state change
                 // 非 book 成交（隐藏单 / 集合竞价），仅打印，不改变簿状态
  Ts       ts;
  SymbolId sym;
  Price    px;
  Qty      qty;
  TradeId  trade_id;
};

struct Clear {   // Wipe the book for this symbol (session reset) / 清空指定 symbol（session 重置）
  Ts       ts;
  SymbolId sym;
};

// Discriminated union over all feed event types.
// 所有行情事件类型的判别联合。
using MdEvent = std::variant<Add, Cancel, Reduce, Exec, Replace, Trade, Clear>;

} // namespace hft::md
