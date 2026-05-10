#include "wire_fixtures.hpp"

namespace hft::test {

Buf enc_header(uint8_t type) {
  Buf b;
  b.put<uint8_t>(type);
  b.put<uint8_t>(0);
  b.put<uint16_t>(0);
  return b;
}

std::vector<std::byte> make_add(hft::Ts ts, hft::SymbolId sym, hft::OrderId id,
                                hft::Side side, hft::Price px, hft::Qty qty) {
  auto b = enc_header(hft::md::wire::kAdd);
  b.put<uint64_t>(ts);
  b.put<uint32_t>(sym);
  b.put<uint64_t>(id);
  b.put<uint8_t>(static_cast<uint8_t>(side));
  b.put<int64_t>(px);
  b.put<int64_t>(qty);
  b.patch_len();
  return std::move(b.data);
}

std::vector<std::byte> make_cancel(hft::Ts ts, hft::SymbolId sym,
                                   hft::OrderId id) {
  auto b = enc_header(hft::md::wire::kCancel);
  b.put<uint64_t>(ts);
  b.put<uint32_t>(sym);
  b.put<uint64_t>(id);
  b.patch_len();
  return std::move(b.data);
}

std::vector<std::byte> make_reduce(hft::Ts ts, hft::SymbolId sym,
                                   hft::OrderId id, hft::Qty new_qty) {
  auto b = enc_header(hft::md::wire::kReduce);
  b.put<uint64_t>(ts);
  b.put<uint32_t>(sym);
  b.put<uint64_t>(id);
  b.put<int64_t>(new_qty);
  b.patch_len();
  return std::move(b.data);
}

std::vector<std::byte> make_exec(hft::Ts ts, hft::SymbolId sym, hft::OrderId id,
                                 hft::Qty exec_qty, hft::Price px,
                                 hft::md::TradeId trade_id) {
  auto b = enc_header(hft::md::wire::kExec);
  b.put<uint64_t>(ts);
  b.put<uint32_t>(sym);
  b.put<uint64_t>(id);
  b.put<int64_t>(exec_qty);
  b.put<int64_t>(px);
  b.put<uint64_t>(trade_id);
  b.patch_len();
  return std::move(b.data);
}

std::vector<std::byte> make_replace(hft::Ts ts, hft::SymbolId sym,
                                    hft::OrderId old_id, hft::OrderId new_id,
                                    hft::Side side, hft::Price px,
                                    hft::Qty qty) {
  auto b = enc_header(hft::md::wire::kReplace);
  b.put<uint64_t>(ts);
  b.put<uint32_t>(sym);
  b.put<uint64_t>(old_id);
  b.put<uint64_t>(new_id);
  b.put<uint8_t>(static_cast<uint8_t>(side));
  b.put<int64_t>(px);
  b.put<int64_t>(qty);
  b.patch_len();
  return std::move(b.data);
}

std::vector<std::byte> make_trade(hft::Ts ts, hft::SymbolId sym, hft::Price px,
                                  hft::Qty qty, hft::md::TradeId trade_id) {
  auto b = enc_header(hft::md::wire::kTrade);
  b.put<uint64_t>(ts);
  b.put<uint32_t>(sym);
  b.put<int64_t>(px);
  b.put<int64_t>(qty);
  b.put<uint64_t>(trade_id);
  b.patch_len();
  return std::move(b.data);
}

std::vector<std::byte> make_clear(hft::Ts ts, hft::SymbolId sym) {
  auto b = enc_header(hft::md::wire::kClear);
  b.put<uint64_t>(ts);
  b.put<uint32_t>(sym);
  b.patch_len();
  return std::move(b.data);
}

std::vector<std::byte>
concat(std::initializer_list<std::vector<std::byte>> frames) {
  std::size_t total = 0;
  for (auto &f : frames)
    total += f.size();
  std::vector<std::byte> out;
  out.reserve(total);
  for (auto &f : frames)
    out.insert(out.end(), f.begin(), f.end());
  return out;
}

} // namespace hft::test