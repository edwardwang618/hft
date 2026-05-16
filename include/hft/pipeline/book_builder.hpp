// include/hft/pipeline/book_builder.hpp
// Multi-symbol book builder pipeline stage.
// 多品种订单簿构建器流水线阶段。
#pragma once

#include <concepts>
#include <cstdlib>
#include <exception>
#include <functional>
#include <hft/core/map_order_book.hpp>
#include <hft/md/md_event.hpp>
#include <optional>
#include <unordered_map>
#include <utility>

namespace hft::pipeline {

// Terminal no-op stage: use when BookBuilder is the last node (strategy path).
// 终端空操作阶段：当 BookBuilder 是流水线末端节点（策略路径）时使用。
struct NullNext {
  template <class BookMap>
  void on_md(uint64_t, const md::MdEvent &, const BookMap &) noexcept {}
};

// Multi-symbol book builder.
// 多品种订单簿构建器。
//
// On every event:
// 每次行情事件：
//   1. Routes the event to the per-symbol book via book.apply(ev).
//      If the book carries a listener/strategy, its callbacks fire here.
//      将事件路由到对应品种的订单簿（book.apply(ev)）。
//      若订单簿携带监听器/策略，回调在此触发。
//   2. Forwards (seq, event, books) to the Next pipeline stage.
//      将 (seq, event, books) 转发到下一个流水线阶段。
//
// Book defaults to MapOrderBook<> (no listener, unbounded price range).
// Book 默认为 MapOrderBook<>（无监听器，价格范围无界）。
// To attach a strategy, pass a factory as the first constructor argument:
// 如需附带策略，将工厂函数作为第一个构造函数参数传入：
//
//   using MyBook = ArrayOrderBook<MyStrategy>;
//   BookBuilder<NullNext, MyBook> builder(
//     [&](md::SymbolId sym) { return MyBook(strategy, sym, min_px, ticks); });
//
// Classic pipeline (no strategy):
// 经典流水线（无策略）：
//   BookBuilder<DownstreamStage> builder(downstream_args...);
//
template <class Next = NullNext, class Book = hft::core::MapOrderBook<>>
class BookBuilder {
public:
  using BookMap = std::unordered_map<md::SymbolId, Book>;
  using BookFactory = std::function<Book(md::SymbolId)>;

  // Without factory: Book must be default-constructible (NullListener books).
  // Disabled when the first argument is convertible to BookFactory so the
  // factory overload below wins for lambdas and std::function arguments.
  // 无工厂：Book 须可默认构造（NullListener 类型）。
  // 当第一个参数可转换为 BookFactory 时禁用，使工厂重载优先匹配
  // lambda/std::function。
  template <class First, class... Rest>
  explicit BookBuilder(First &&first, Rest &&...rest)
    requires(!std::convertible_to<std::decay_t<First>, BookFactory>)
      : next_(std::forward<First>(first), std::forward<Rest>(rest)...) {}

  explicit BookBuilder() : next_() {}

  // With factory: for books that need a listener bound at construction.
  // 带工厂：用于构造时需绑定监听器的 Book。
  template <class... A>
  explicit BookBuilder(BookFactory factory, A &&...a)
      : factory_(std::move(factory)), next_(std::forward<A>(a)...) {}

  void on_md(uint64_t seq, const md::MdEvent &e) noexcept {
    std::visit([&](const auto &ev) { dispatch(ev); }, e);
    next_.on_md(seq, e, books_);
  }

  const BookMap &books() const noexcept { return books_; }

  // Single-symbol lookup; returns nullptr if the symbol has not been seen yet
  // or was removed by a Clear event.
  // 单品种查找；若该品种尚未出现或已被 Clear 移除则返回 nullptr。
  const Book *book(md::SymbolId sym) const noexcept {
    auto it = books_.find(sym);
    return it != books_.end() ? &it->second : nullptr;
  }

  Next &next() noexcept { return next_; }
  const Next &next() const noexcept { return next_; }

private:
  std::optional<BookFactory> factory_;
  BookMap books_;
  Next next_;

  Book &book_of(md::SymbolId sym) {
    auto it = books_.find(sym);
    if (it != books_.end())
      return it->second;
    if (factory_)
      return books_.emplace(sym, (*factory_)(sym)).first->second;
    // Fallback: default-construct. Only reachable when Book is
    // default-constructible (NullListener books); factory-required books
    // always hit the branch above.
    // 回退：默认构造。仅在 Book 可默认构造时可达；需要工厂的 Book
    // 始终命中上方分支。
    if constexpr (std::default_initializable<Book>)
      return books_.emplace(sym, Book{}).first->second;
    else
      std::terminate(); // factory must be provided for this Book type / 此 Book
                        // 类型必须提供工厂
  }

  // Generic: apply the event (fires listener callbacks if book has one).
  // 通用分发：应用事件（若 Book 有监听器则触发回调）。
  template <class Ev> void dispatch(const Ev &ev) { book_of(ev.sym).apply(ev); }

  // Clear fires the listener's on_bbo (empty BBO) then removes the book entry.
  // Clear 先触发监听器的 on_bbo（空 BBO），然后从 map 中移除该品种簿。
  void dispatch(const md::Clear &c) {
    auto it = books_.find(c.sym);
    if (it != books_.end()) {
      it->second.apply(c);
      books_.erase(it);
    }
  }
};

} // namespace hft::pipeline
