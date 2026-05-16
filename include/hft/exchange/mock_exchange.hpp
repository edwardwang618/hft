#pragma once
#include <cstddef>
#include <cstdint>
#include <hft/exchange/byte_sink.hpp>
#include <hft/exchange/wire_serializer.hpp>
#include <hft/md/md_event.hpp>
#include <vector>

namespace hft::exchange {

// Scripted mock exchange:
//   - 用户 push(MdEvent) 预先排好节目单
//   - run() 按顺序给每条消息分配一个递增 seq, 编码, 喂给 sink
//   - 保留 "真相 book 事件流" 供测试对照
class MockExchange {
public:
  MockExchange(IByteSink &sink, IWireSerializer &ser,
               std::uint64_t start_seq = 1) noexcept
      : sink_(sink), ser_(ser), next_seq_(start_seq) {
    buf_.resize(ser.max_frame_size());
  }

  // 排进节目单, 还没发.
  void push(md::MdEvent ev) { script_.push_back(std::move(ev)); }

  // 一次性发完.
  void run() noexcept {
    for (const auto &ev : script_)
      emit(ev);
  }

  // 单条直接发 (流式用法, 之后 stochastic / chaos 模式复用).
  void emit(const md::MdEvent &ev) noexcept {
    const auto seq = next_seq_++;
    const auto n = ser_.encode(ev, seq, buf_.data(), buf_.size());
    if (n == 0)
      return; // buffer 不够, 静默丢; 生产环境应该 assert
    sink_.on_bytes(buf_.data(), n);
    ++msgs_sent_;
  }

  // 观测
  std::uint64_t msgs_sent() const noexcept { return msgs_sent_; }
  std::uint64_t next_seq() const noexcept { return next_seq_; }
  const std::vector<md::MdEvent> &script() const noexcept { return script_; }

  void clear_script() noexcept { script_.clear(); }

private:
  IByteSink &sink_;
  IWireSerializer &ser_;
  std::vector<md::MdEvent> script_;
  std::vector<std::byte> buf_;
  std::uint64_t next_seq_;
  std::uint64_t msgs_sent_ = 0;
};

} // namespace hft::exchange