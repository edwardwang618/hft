#pragma once

#include <hft/core/order_book.hpp>
#include <hft/md/md_event.hpp>
#include <unordered_map>
#include <utility>

namespace hft::pipeline {

// 多 symbol: 每个 sym 一个 OrderBook.
// Next 需要提供: void on_md(const MdEvent&, const BookMap&) noexcept
// 让下游 stage 能拿到 book 快照 (const view).
template <class Next> class BookBuilder {
public:
  using Book = hft::core::OrderBook;
  using BookMap = std::unordered_map<md::SymbolId, Book>;

  template <class... A>
  explicit BookBuilder(A &&...a) : next_(std::forward<A>(a)...) {}

  void on_md(std::uint64_t seq, const md::MdEvent &e) noexcept {
    std::visit([this](const auto &ev) { apply(ev); }, e);
    next_.on_md(seq, e, books_);
  }

  const BookMap &books() const noexcept { return books_; }

  Next &next() noexcept { return next_; }
  const Next &next() const noexcept { return next_; }

private:
  BookMap books_;
  Next next_;

  Book &book_of(md::SymbolId s) { return books_[s]; } // 首次访问自动建

  void apply(const md::Add &a) {
    hft::core::Order o{};
    o.id = a.id;
    o.side = a.side;
    o.price = a.px;
    o.qty = a.qty;
    o.ts = a.ts;
    book_of(a.sym).add_limit(o);
  }
  void apply(const md::Cancel &c) { book_of(c.sym).cancel(c.id); }
  void apply(const md::Reduce &r) { book_of(r.sym).reduce(r.id, r.new_qty); }
  void apply(const md::Exec &x) { book_of(x.sym).execute(x.id, x.exec_qty); }
  void apply(const md::Replace &r) {
    auto &b = book_of(r.sym);
    b.cancel(r.old_id);
    hft::core::Order o{};
    o.id = r.new_id;
    o.price = r.px;
    o.qty = r.qty;
    o.ts = r.ts;
    // side 从原单继承 — 但我们已经 cancel 了, 拿不到. 约定: Replace 事件带
    // side. → 改 md_event.hpp: Replace 加 Side side;
    b.add_limit(o);
  }
  void apply(const md::Trade &) { /* 非 book, 透传即可 */ }
  void apply(const md::Clear &c) { books_.erase(c.sym); }
};

} // namespace hft::pipeline