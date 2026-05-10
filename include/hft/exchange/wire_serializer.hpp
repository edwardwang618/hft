#pragma once
#include <cstddef>
#include <cstdint>
#include <hft/md/md_event.hpp>

namespace hft::exchange {

// 把 MdEvent + seq 编码成 bytes. 不同 wire (binary / FIX) 实现不同 serializer.
// 返回写入字节数; 若 cap 不够返回 0 (调用方要给够大 buffer).
struct IWireSerializer {
  virtual std::size_t encode(const md::MdEvent &ev, std::uint64_t seq,
                             std::byte *out, std::size_t cap) noexcept = 0;

  // 最大可能帧长, 调用方据此 size buffer.
  virtual std::size_t max_frame_size() const noexcept = 0;

  virtual ~IWireSerializer() = default;
};

} // namespace hft::exchange