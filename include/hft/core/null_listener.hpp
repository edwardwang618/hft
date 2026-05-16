// include/hft/core/null_listener.hpp
// Default no-op listener; satisfies the Listener concept with zero overhead.
// 默认空监听器；以零开销满足 Listener 接口约束。
#pragma once

#include <hft/core/types.hpp>
#include <hft/md/md_event.hpp>

namespace hft::core::detail {

// Used when no strategy is attached to a book.
// 当订单簿不附带策略时使用。
struct NullListener {
  void on_bbo(md::SymbolId, Price, Qty, Price, Qty) noexcept {}
  template <class Book>
  void on_depth(md::SymbolId, const Book &) noexcept {}
  void on_trade(md::SymbolId, Price, Qty, md::TradeId) noexcept {}
};

inline NullListener g_null_listener;

} // namespace hft::core::detail
