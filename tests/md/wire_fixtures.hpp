#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <span>
#include <vector>

#include <hft/core/types.hpp>
#include <hft/md/md_event.hpp>
#include <hft/md/wire_binary.hpp>

namespace hft::test {

struct Buf {
  std::vector<std::byte> data;
  template <class T> void put(const T &v) {
    const auto *p = reinterpret_cast<const std::byte *>(&v);
    data.insert(data.end(), p, p + sizeof(T));
  }
  std::size_t size() const { return data.size(); }
  void patch_len() {
    auto len = static_cast<uint16_t>(data.size());
    std::memcpy(data.data() + 2, &len, sizeof(len));
  }
  std::span<const std::byte> span() const { return {data.data(), data.size()}; }
};

Buf enc_header(uint8_t type, uint64_t seq = 0);

std::vector<std::byte> make_add(uint64_t seq, hft::Ts ts, hft::SymbolId sym,
                                hft::OrderId id, hft::Side side, hft::Price px,
                                hft::Qty qty);

std::vector<std::byte> make_cancel(uint64_t seq, hft::Ts ts, hft::SymbolId sym,
                                   hft::OrderId id);

std::vector<std::byte> make_reduce(uint64_t seq, hft::Ts ts, hft::SymbolId sym,
                                   hft::OrderId id, hft::Qty new_qty);

std::vector<std::byte> make_exec(uint64_t seq, hft::Ts ts, hft::SymbolId sym,
                                 hft::OrderId id, hft::Qty exec_qty,
                                 hft::Price px, hft::md::TradeId trade_id);

std::vector<std::byte> make_replace(uint64_t seq, hft::Ts ts, hft::SymbolId sym,
                                    hft::OrderId old_id, hft::OrderId new_id,
                                    hft::Side side, hft::Price px,
                                    hft::Qty qty);

std::vector<std::byte> make_trade(uint64_t seq, hft::Ts ts, hft::SymbolId sym,
                                  hft::Price px, hft::Qty qty,
                                  hft::md::TradeId trade_id);

std::vector<std::byte> make_clear(uint64_t seq, hft::Ts ts, hft::SymbolId sym);

std::vector<std::byte>
concat(std::initializer_list<std::vector<std::byte>> frames);

} // namespace hft::test