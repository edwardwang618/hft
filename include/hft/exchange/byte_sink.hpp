#pragma once
#include <cstddef>
#include <cstdint>

namespace hft::exchange {

// 上游把编码好的字节推给下游的抽象. 实现可以是:
//   - DirectSink: 直接调 FeedHandler::on_bytes (测试用)
//   - SpscSink:   写入 SPSC ring (跨线程)
//   - UdpSink:    以后加
struct IByteSink {
  virtual void on_bytes(const std::byte *data, std::size_t n) noexcept = 0;
  virtual ~IByteSink() = default;
};

// 最简实现: 直接调下游的 on_bytes. 模板避免虚调用.
template <class Downstream> class DirectSink final : public IByteSink {
public:
  explicit DirectSink(Downstream &d) noexcept : d_(d) {}
  void on_bytes(const std::byte *data, std::size_t n) noexcept override {
    d_.on_bytes(data, n);
  }

private:
  Downstream &d_;
};

} // namespace hft::exchange