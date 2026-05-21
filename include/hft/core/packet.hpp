// include/hft/core/packet.hpp
// Fixed-size packet: the unit transferred through the network→hot-path SPSC
// ring. 固定大小数据包：网络线程→热路径 SPSC ring 的传输单元。
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace hft::core {

// MaxLen defaults to 2048 — fits one standard Ethernet MTU (1500 B) with room.
// Fixed size so SpscRing<Packet<N>, Cap> uses its trivially-copyable fast path.
// 固定大小使 SpscRing 可直接 memcpy，无需虚函数或动态分配。
template <std::size_t MaxLen = 2048> struct Packet {
  static constexpr std::size_t kMaxLen = MaxLen;
  std::array<std::byte, MaxLen> data{};
  uint32_t len{0};
};

static_assert(std::is_trivially_copyable_v<Packet<>>);

} // namespace hft::core