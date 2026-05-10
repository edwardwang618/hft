#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <span>
#include <vector>

#include <hft/core/types.hpp>     // hft::Ts / OrderId / Price / Qty / Side
#include <hft/md/md_event.hpp>    // hft::md::TradeId (SymbolId 也在 hft::)
#include <hft/md/wire_binary.hpp> // hft::md::wire::kAdd ...

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

Buf enc_header(uint8_t type);

std::vector<std::byte> make_add(hft::Ts ts, hft::SymbolId sym, hft::OrderId id,
                                hft::Side side, hft::Price px, hft::Qty qty);

std::vector<std::byte> make_cancel(hft::Ts ts, hft::SymbolId sym,
                                   hft::OrderId id);

std::vector<std::byte> make_reduce(hft::Ts ts, hft::SymbolId sym,
                                   hft::OrderId id, hft::Qty new_qty);

std::vector<std::byte> make_exec(hft::Ts ts, hft::SymbolId sym, hft::OrderId id,
                                 hft::Qty exec_qty, hft::Price px,
                                 hft::md::TradeId trade_id);

std::vector<std::byte> make_replace(hft::Ts ts, hft::SymbolId sym,
                                    hft::OrderId old_id, hft::OrderId new_id,
                                    hft::Side side, hft::Price px,
                                    hft::Qty qty);

std::vector<std::byte> make_trade(hft::Ts ts, hft::SymbolId sym, hft::Price px,
                                  hft::Qty qty, hft::md::TradeId trade_id);

std::vector<std::byte> make_clear(hft::Ts ts, hft::SymbolId sym);

std::vector<std::byte>
concat(std::initializer_list<std::vector<std::byte>> frames);

} // namespace hft::test