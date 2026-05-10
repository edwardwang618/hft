#pragma once

#include <hft/md/md_event.hpp>
#include <cstdint>

namespace hft::pipeline {

// ============================================================
// Stage 约定 (concept, 非强制 — 只是文档):
//
//   入口 stage (消费字节):
//     void on_bytes(const std::byte*, std::size_t);
//     内部调用 next_.on_md(seq, event);
//
//   中间 stage A 类 (不携带 book):
//     void on_md(std::uint64_t seq, const md::MdEvent& ev) noexcept;
//     内部: next_.on_md(seq, ev);
//
//   中间 stage B 类 (携带 book, 典型: BookBuilder):
//     void on_md(std::uint64_t seq, const md::MdEvent& ev) noexcept;
//     内部: next_.on_md(seq, ev, books_);
//     → 下游 stage 必须接 (seq, ev, BookMap&) 三参形式.
//
//   终结 stage:
//     提供匹配上游的 on_md, 不再转发.
//
// 装配方式: 直接类型别名嵌套.
//   using Pipe = FeedHandler< BookBuilder< StrategyStage<MyStrat,
//                                                         NullBookSink> > >;
// ============================================================

// 终结 stage: 接在 FeedHandler 之后 (没经过 BookBuilder).
struct NullEventSink {
  void on_md(std::uint64_t, const md::MdEvent&) noexcept {}
};

// 终结 stage: 接在 BookBuilder 之后.
template <class BookMap>
struct NullBookSink {
  void on_md(std::uint64_t, const md::MdEvent&, const BookMap&) noexcept {}
};

} // namespace hft::pipeline