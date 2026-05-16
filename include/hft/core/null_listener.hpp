#pragma once

#include <hft/core/types.hpp>
#include <hft/md/md_event.hpp>

namespace hft::core::detail {

// Default no-op listener used when no strategy is attached.
// Satisfies the Listener concept required by both MapOrderBook and ArrayOrderBook.
struct NullListener {
  void on_bbo(md::SymbolId, Price, Qty, Price, Qty) noexcept {}
  template <class Book>
  void on_depth(md::SymbolId, const Book &) noexcept {}
  void on_trade(md::SymbolId, Price, Qty, md::TradeId) noexcept {}
};

inline NullListener g_null_listener;

} // namespace hft::core::detail
