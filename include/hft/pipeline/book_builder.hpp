#pragma once

#include <hft/core/map_order_book.hpp>
#include <hft/md/md_event.hpp>
#include <concepts>
#include <cstdlib>
#include <functional>
#include <optional>
#include <unordered_map>
#include <utility>

namespace hft::pipeline {

// Terminal no-op stage: use when BookBuilder is the last node (strategy path).
struct NullNext {
  template <class BookMap>
  void on_md(uint64_t, const md::MdEvent &, const BookMap &) noexcept {}
};

// Multi-symbol book builder.
//
// On every event:
//   1. Routes the event to the per-symbol book via book.apply(ev).
//      If the book carries a listener/strategy, its callbacks fire here.
//   2. Forwards (seq, event, books) to the Next pipeline stage.
//
// Book defaults to MapOrderBook<> (no listener, unbounded price range).
// To attach a strategy, pass a factory as the first constructor argument:
//
//   using MyBook = ArrayOrderBook<MyStrategy>;
//   BookBuilder<NullNext, MyBook> builder(
//     [&](md::SymbolId sym) { return MyBook(strategy, sym, min_px, ticks); });
//
// Classic pipeline (no strategy):
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
  template <class First, class... Rest>
  explicit BookBuilder(First &&first, Rest &&...rest)
      requires(!std::convertible_to<std::decay_t<First>, BookFactory>)
      : next_(std::forward<First>(first), std::forward<Rest>(rest)...) {}

  explicit BookBuilder() : next_() {}

  // With factory: for books that need a listener bound at construction.
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
    if constexpr (std::default_initializable<Book>)
      return books_.emplace(sym, Book{}).first->second;
    else
      std::terminate(); // factory must be provided for this Book type
  }

  // Generic: apply the event (fires listener callbacks if book has one).
  template <class Ev>
  void dispatch(const Ev &ev) {
    book_of(ev.sym).apply(ev);
  }

  // Clear fires the listener's on_bbo (empty BBO) then removes the book entry.
  void dispatch(const md::Clear &c) {
    auto it = books_.find(c.sym);
    if (it != books_.end()) {
      it->second.apply(c);
      books_.erase(it);
    }
  }
};

} // namespace hft::pipeline
