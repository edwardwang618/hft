// tests/integration/test_end_to_end.cpp
//
// End-to-end integration test: exchange broadcasts market data (both binary and
// FIX protocols), trading system runs the full pipeline
//   FeedHandler → GapDetector → BookBuilder → SimpleQuoter
// then sends orders back through:
//   DeferredGateway → RiskGateway → OrderGatewayT<InProcessSession>
// which routes OE bytes to an in-process ExchangeSimulator.
// The simulator acks orders and can trigger fills on demand.
//
// 端到端集成测试：交易所以 Binary / FIX 两种协议广播行情，
// 交易系统走完全流水线后发出 OE 订单并收到交易所回报。

#include <hft/core/map_order_book.hpp>
#include <hft/core/types.hpp>
#include <hft/exchange/binary_serializer.hpp>
#include <hft/exchange/byte_sink.hpp>
#include <hft/exchange/fix_serializer.hpp>
#include <hft/exchange/in_process_session.hpp>
#include <hft/exchange/mock_exchange.hpp>
#include <hft/md/binary_parser.hpp>
#include <hft/md/fix_parser.hpp>
#include <hft/md/md_event.hpp>
#include <hft/oe/order_gateway.hpp>
#include <hft/oe/wire_oe.hpp>
#include <hft/pipeline/book_builder.hpp>
#include <hft/pipeline/feed_handler.hpp>
#include <hft/pipeline/gap_detector.hpp>
#include <hft/risk/risk_gateway.hpp>
#include <hft/strategy/i_order_gateway.hpp>
#include <hft/strategy/i_order_listener.hpp>
#include <hft/strategy/simple_quoter.hpp>

#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

using namespace hft;

// ── TestListener ─────────────────────────────────────────────────────────────
// Records all execution report callbacks for assertion.
// 记录所有回报回调，供断言使用。

struct TestListener final : strategy::IOrderListener {
  struct AckInfo   { OrderId clord_id, exch_id; };
  struct FillInfo  { OrderId exch_id; Qty qty; Price px; };
  struct CxlInfo   { OrderId exch_id; };
  struct RejectInfo{ OrderId clord_id; std::string reason; };

  std::vector<AckInfo>    acks;
  std::vector<FillInfo>   fills;
  std::vector<CxlInfo>    cancel_acks;
  std::vector<RejectInfo> rejects;

  void on_ack(OrderId c, OrderId e) override { acks.push_back({c, e}); }
  void on_fill(OrderId e, Qty q, Price p) override { fills.push_back({e, q, p}); }
  void on_cancel_ack(OrderId e) override { cancel_acks.push_back({e}); }
  void on_reject(OrderId c, std::string_view r) override {
    rejects.push_back({c, std::string(r)});
  }
};

// ── DeferredGateway ───────────────────────────────────────────────────────────
// IOrderGateway stub that forwards to a real gateway once bound.
// Breaks the circular construction dependency: strategy needs a gateway at
// construction, but the gateway chain needs the strategy (via the book factory).
//
// 转发桩：在绑定真实网关前充当占位符，解决循环构造依赖问题：
// 策略构造时需要网关，但网关链通过 book factory 又依赖策略。

class DeferredGateway final : public strategy::IOrderGateway {
public:
  void bind(strategy::IOrderGateway &gw) noexcept { real_ = &gw; }

  OrderId send_new(md::SymbolId sym, Side side, Price px, Qty qty) override {
    return real_ ? real_->send_new(sym, side, px, qty) : kInvalidOrderId;
  }
  void send_cancel(OrderId id) override {
    if (real_) real_->send_cancel(id);
  }
  void send_replace(OrderId id, Price px, Qty qty) override {
    if (real_) real_->send_replace(id, px, qty);
  }

private:
  strategy::IOrderGateway *real_{nullptr};
};

// ── ExchangeSimulator ─────────────────────────────────────────────────────────
// Minimal in-process exchange: decodes OE command bytes from the trading system,
// acks every new order, and can trigger fills on demand.
//
// 最小化进程内交易所：解码交易系统发来的 OE 命令字节，对每笔新单立即回 Ack，
// 并可按需触发 Fill。

class ExchangeSimulator final : public exchange::IByteSink {
public:
  void bind_reports(exchange::IByteSink &sink) noexcept { report_ = &sink; }

  // Manually send a Fill for a strategy order identified by clord_id.
  // 根据 clord_id 手动触发策略订单的 Fill 回报。
  void fill_strategy_order(OrderId clord_id, Qty qty, Price px) {
    auto it = orders_.find(clord_id);
    if (it == orders_.end()) return;
    send_fill(clord_id, it->second.exch_id, qty, px);
  }

  // IByteSink: receives encoded OE command bytes from the trading system.
  // IByteSink：接收交易系统发来的编码 OE 命令字节。
  void on_bytes(const std::byte *data, std::size_t n) noexcept override {
    buf_.insert(buf_.end(), data, data + n);
    drain();
  }

private:
  struct StrategyOrder { OrderId exch_id; };

  exchange::IByteSink *report_{nullptr};
  std::vector<std::byte> buf_;
  std::unordered_map<OrderId, StrategyOrder> orders_; // clord_id → order info
  uint64_t next_exch_id_{1};

  void drain() {
    std::size_t offset = 0;
    while (buf_.size() - offset >= sizeof(oe::wire::Header)) {
      oe::wire::Header hdr{};
      std::memcpy(&hdr, buf_.data() + offset, sizeof(hdr));
      if (hdr.msg_len < sizeof(oe::wire::Header) || hdr.msg_len > 512) {
        buf_.clear(); // corrupt stream — reset and give up
        return;
      }
      if (buf_.size() - offset < hdr.msg_len) break;

      if (hdr.msg_type == oe::wire::kNewOrder &&
          hdr.msg_len >= sizeof(oe::wire::NewOrder)) {
        oe::wire::NewOrder msg{};
        std::memcpy(&msg, buf_.data() + offset, sizeof(msg));
        on_new_order(msg);
      }
      offset += hdr.msg_len;
    }
    if (offset > 0)
      buf_.erase(buf_.begin(),
                 buf_.begin() + static_cast<std::ptrdiff_t>(offset));
  }

  void on_new_order(const oe::wire::NewOrder &msg) {
    const OrderId exch_id = next_exch_id_++;
    orders_[msg.hdr.clord_id] = {exch_id};
    send_ack(msg.hdr.clord_id, exch_id);
  }

  void send_ack(OrderId clord_id, OrderId exch_id) {
    if (!report_) return;
    oe::wire::Ack ack{};
    ack.hdr.msg_type = oe::wire::kAck;
    ack.hdr.msg_len  = sizeof(oe::wire::Ack);
    ack.hdr.clord_id = clord_id;
    ack.exch_id      = exch_id;
    report_->on_bytes(reinterpret_cast<const std::byte *>(&ack), sizeof(ack));
  }

  void send_fill(OrderId clord_id, OrderId exch_id, Qty qty, Price px) {
    if (!report_) return;
    oe::wire::Fill fill{};
    fill.hdr.msg_type  = oe::wire::kFill;
    fill.hdr.msg_len   = sizeof(oe::wire::Fill);
    fill.hdr.clord_id  = clord_id;
    fill.exch_id       = exch_id;
    fill.filled_qty    = qty;
    fill.px            = px;
    report_->on_bytes(reinterpret_cast<const std::byte *>(&fill), sizeof(fill));
  }
};

// ── Shared test scenario (templated on MD wire parser) ────────────────────────
//
// All steps happen synchronously in-process:
//   md_exchange.emit(Add{Sell@101}) → pipeline → quoter.on_bbo(bid=0, ask=101)
//     → send_new(Sell@101) → ExchangeSimulator → Ack → OeDecoder → listener
//   md_exchange.emit(Add{Buy@99})   → pipeline → quoter.on_bbo(bid=99, ask=101)
//     → send_new(Buy@99)  → ExchangeSimulator → Ack → OeDecoder → listener
//   exchange.fill_strategy_order(bid_clord)
//     → Fill → OeDecoder → listener
//
// 所有步骤均在进程内同步完成（见注释）。

template <class Parser, class Serializer>
static void run_scenario(Serializer &ser) {
  constexpr md::SymbolId kSym = 1;

  // ── 1. In-process exchange (OE command receiver + report sender) ──────────
  ExchangeSimulator exchange;

  // ── 2. Strategy (needs a gateway; use deferred stub) ─────────────────────
  DeferredGateway deferred_gw;
  strategy::SimpleQuoter quoter(deferred_gw, {kSym, 1});

  // ── 3. Pipeline: BookBuilder → GapDetector → FeedHandler ─────────────────
  using StrategyBook = core::MapOrderBook<strategy::SimpleQuoter>;
  pipeline::BookBuilder<pipeline::NullNext, StrategyBook> bb(
      [&](md::SymbolId sym) { return StrategyBook(quoter, sym); });

  pipeline::GapDetector<decltype(bb)> gd(bb, [](uint64_t, uint64_t) {});
  pipeline::FeedHandler<decltype(gd), Parser> fh(gd);

  // ── 4. MD broadcaster (exchange → trading system) ─────────────────────────
  exchange::DirectSink<decltype(fh)> md_sink(fh);
  exchange::MockExchange md_exchange(md_sink, ser);

  // ── 5. OE path (trading system → exchange → reports back) ─────────────────
  TestListener oe_listener;
  exchange::DirectSink<ExchangeSimulator> oe_cmd_sink(exchange);
  exchange::InProcessSession oe_session(oe_cmd_sink);
  oe::OrderGatewayT<exchange::InProcessSession> gw(oe_listener, oe_session);
  risk::RiskGateway risk_gw(gw, {});
  deferred_gw.bind(risk_gw);
  exchange.bind_reports(gw.report_sink());

  // ── Scenario step 1: exchange broadcasts Sell@101 ─────────────────────────
  // BBO: (bid=0, ask=101) → quoter sends ask@101 → Ack
  md_exchange.emit(md::Add{0, kSym, /*id=*/1, Side::Sell, 101 * kPriceScale, 100});
  ASSERT_EQ(oe_listener.acks.size(), 1u) << "ask order was not acked";
  EXPECT_EQ(quoter.ask_px(), 101 * kPriceScale);
  EXPECT_EQ(quoter.bid_id(), kInvalidOrderId); // no bid yet

  // ── Scenario step 2: exchange broadcasts Buy@99 ───────────────────────────
  // BBO: (bid=99, ask=101) → quoter sends bid@99 → Ack
  md_exchange.emit(md::Add{0, kSym, /*id=*/2, Side::Buy, 99 * kPriceScale, 100});
  ASSERT_EQ(oe_listener.acks.size(), 2u) << "bid order was not acked";
  EXPECT_EQ(quoter.bid_px(), 99 * kPriceScale);

  // ── Scenario step 3: exchange fills the strategy's bid order ──────────────
  const OrderId bid_clord = quoter.bid_id();
  ASSERT_NE(bid_clord, kInvalidOrderId);
  exchange.fill_strategy_order(bid_clord, /*qty=*/1, /*px=*/99 * kPriceScale);
  ASSERT_EQ(oe_listener.fills.size(), 1u) << "fill was not delivered";
  EXPECT_EQ(oe_listener.fills[0].qty, 1);
  EXPECT_EQ(oe_listener.fills[0].px,  99 * kPriceScale);
}

// ── Test cases ────────────────────────────────────────────────────────────────

TEST(EndToEnd, BinaryProtocol) {
  exchange::BinarySerializer ser;
  run_scenario<md::BinaryParser>(ser);
}

TEST(EndToEnd, FixProtocol) {
  exchange::FixSerializer ser;
  run_scenario<md::FixParser>(ser);
}
