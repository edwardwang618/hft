#pragma once
#include <cstring>
#include <hft/exchange/wire_serializer.hpp>
#include <hft/md/wire_binary.hpp>

namespace hft::exchange {

class BinarySerializer final : public IWireSerializer {
public:
  std::size_t max_frame_size() const noexcept override {
    return 64;
  } // Replace=57

  std::size_t encode(const md::MdEvent &ev, std::uint64_t seq, std::byte *out,
                     std::size_t cap) noexcept override {
    return std::visit(
        [&](const auto &e) { return encode_one(e, seq, out, cap); }, ev);
  }

private:
  using H = md::wire::Header;

  template <class T> static void put(std::byte *&p, const T &v) noexcept {
    std::memcpy(p, &v, sizeof(T));
    p += sizeof(T);
  }

  // 每个 overload: 写 header (type, len, seq) + body. 字段顺序严格和 wire doc
  // 一致.
  std::size_t encode_one(const md::Add &e, std::uint64_t seq, std::byte *out,
                         std::size_t cap) noexcept {
    constexpr std::size_t n = sizeof(H) + 37;
    if (cap < n)
      return 0;
    std::byte *p = out;
    H h{md::wire::kAdd, 0, static_cast<uint16_t>(n), seq};
    put(p, h);
    put(p, e.ts);
    put(p, e.sym);
    put(p, e.id);
    put(p, static_cast<uint8_t>(e.side));
    put(p, e.px);
    put(p, e.qty);
    return n;
  }

  std::size_t encode_one(const md::Cancel &e, std::uint64_t seq, std::byte *out,
                         std::size_t cap) noexcept {
    constexpr std::size_t n = sizeof(H) + 20;
    if (cap < n)
      return 0;
    std::byte *p = out;
    H h{md::wire::kCancel, 0, static_cast<uint16_t>(n), seq};
    put(p, h);
    put(p, e.ts);
    put(p, e.sym);
    put(p, e.id);
    return n;
  }

  std::size_t encode_one(const md::Reduce &e, std::uint64_t seq, std::byte *out,
                         std::size_t cap) noexcept {
    constexpr std::size_t n = sizeof(H) + 28;
    if (cap < n)
      return 0;
    std::byte *p = out;
    H h{md::wire::kReduce, 0, static_cast<uint16_t>(n), seq};
    put(p, h);
    put(p, e.ts);
    put(p, e.sym);
    put(p, e.id);
    put(p, e.new_qty);
    return n;
  }

  std::size_t encode_one(const md::Exec &e, std::uint64_t seq, std::byte *out,
                         std::size_t cap) noexcept {
    constexpr std::size_t n = sizeof(H) + 44;
    if (cap < n)
      return 0;
    std::byte *p = out;
    H h{md::wire::kExec, 0, static_cast<uint16_t>(n), seq};
    put(p, h);
    put(p, e.ts);
    put(p, e.sym);
    put(p, e.id);
    put(p, e.exec_qty);
    put(p, e.px);
    put(p, e.trade_id);
    return n;
  }

  std::size_t encode_one(const md::Replace &e, std::uint64_t seq,
                         std::byte *out, std::size_t cap) noexcept {
    constexpr std::size_t n = sizeof(H) + 45;
    if (cap < n)
      return 0;
    std::byte *p = out;
    H h{md::wire::kReplace, 0, static_cast<uint16_t>(n), seq};
    put(p, h);
    put(p, e.ts);
    put(p, e.sym);
    put(p, e.old_id);
    put(p, e.new_id);
    put(p, static_cast<uint8_t>(e.side));
    put(p, e.px);
    put(p, e.qty);
    return n;
  }

  std::size_t encode_one(const md::Trade &e, std::uint64_t seq, std::byte *out,
                         std::size_t cap) noexcept {
    constexpr std::size_t n = sizeof(H) + 36;
    if (cap < n)
      return 0;
    std::byte *p = out;
    H h{md::wire::kTrade, 0, static_cast<uint16_t>(n), seq};
    put(p, h);
    put(p, e.ts);
    put(p, e.sym);
    put(p, e.px);
    put(p, e.qty);
    put(p, e.trade_id);
    return n;
  }

  std::size_t encode_one(const md::Clear &e, std::uint64_t seq, std::byte *out,
                         std::size_t cap) noexcept {
    constexpr std::size_t n = sizeof(H) + 12;
    if (cap < n)
      return 0;
    std::byte *p = out;
    H h{md::wire::kClear, 0, static_cast<uint16_t>(n), seq};
    put(p, h);
    put(p, e.ts);
    put(p, e.sym);
    return n;
  }
};

} // namespace hft::exchange