#pragma once
#include <hft/md/md_event.hpp>
#include <utility>

namespace hft::pipeline {

// Terminal: 链尾, 什么都不做. 写策略 / 测试时用.
struct Sink {
  void on_md(const md::MdEvent &) noexcept {}
};

// make_pipeline(StageA{}, StageB{}, ..., Sink{})
// 按参数顺序从左到右嵌套: StageA<StageB<...<Sink>>>
// 每个 stage 必须是: template<class Next> class X { ... ; Next next_; };
// 且提供 void on_md(const MdEvent&) 转发给 next_.

namespace detail {

template <class Last> constexpr auto build(Last &&last) {
  return std::forward<Last>(last);
}

template <template <class> class Head, class... Rest> struct Tag {};

// 用户写: make_pipeline<BookBuilder, StrategyStage<S>::tpl,
// ...>(args_for_each_stage...) 这个写法太绕. 换一种: 用户自己写 using Pipe =
// BookBuilder<Strat<Sink>>; 下面 make_pipeline 只是把 Sink 作为默认尾.

} // namespace detail

// 推荐写法: 不做花哨工厂, 直接嵌套类型别名. 例:
//   using Pipe = BookBuilder< StrategyStage<MyStrat, Sink> >;
//   Pipe p{ MyStrat{...} };
// 每个 stage 的构造函数签名自己定.

} // namespace hft::pipeline