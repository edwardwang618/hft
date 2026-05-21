// include/hft/exchange/spsc_sink.hpp
// IByteSink that queues raw packets into a SpscRing for the hot-path thread.
// 将原始字节包入队到 SpscRing，供热路径线程消费的 IByteSink 实现。
//
// Threading:
//   Producer (network thread) : on_bytes() — called by UdpReceiver
//   Consumer (hot-path thread): try_pop()  — feeds FeedHandler::on_bytes()
//
// 线程模型：
//   生产者（网络线程）：on_bytes() — 由 UdpReceiver 调用
//   消费者（热路径线程）：try_pop() — 将包喂给 FeedHandler::on_bytes()
#pragma once

#include <hft/core/packet.hpp>
#include <hft/core/spsc_ring.hpp>
#include <hft/exchange/byte_sink.hpp>

#include <atomic>
#include <cstring>

namespace hft::exchange {

// N        : ring capacity (must be power of 2)
// MaxLen   : max bytes per packet (default 2 KiB)
template <std::size_t N, std::size_t MaxLen = 2048>
class SpscSink final : public IByteSink {
public:
  using PacketT = core::Packet<MaxLen>;

  // Producer: copy bytes into a free ring slot and publish.
  // Oversized or ring-full packets are counted and silently dropped.
  // 生产者：将字节拷贝到空闲 ring 槽并发布。超长或 ring 满时静默丢弃并计数。
  void on_bytes(const std::byte *data, std::size_t len) noexcept override {
    if (len > MaxLen) {
      dropped_.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    PacketT pkt;
    std::memcpy(pkt.data.data(), data, len);
    pkt.len = static_cast<uint32_t>(len);
    if (!ring_.try_push(pkt))
      dropped_.fetch_add(1, std::memory_order_relaxed);
  }

  // Consumer: pop one packet. Returns false if the ring is empty.
  // 消费者：弹出一个包。ring 为空时返回 false。
  bool try_pop(PacketT &out) noexcept { return ring_.try_pop(out); }

  // Packets dropped since construction (ring full or oversized).
  // Relaxed: diagnostic counter, exact value not required.
  // 自构造以来丢弃的包数（ring 满或超长）。Relaxed：诊断计数，无需精确。
  uint64_t dropped() const noexcept {
    return dropped_.load(std::memory_order_relaxed);
  }

private:
  core::SpscRing<PacketT, N> ring_;
  std::atomic<uint64_t> dropped_{0};
};

} // namespace hft::exchange
