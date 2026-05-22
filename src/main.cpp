// src/main.cpp
// Production entry point: wires the full HFT pipeline for a single symbol.
// 生产环境入口点：为单一品种组装完整 HFT 流水线。
//
// Threading model / 线程模型:
//   io_thread  (core 1, SCHED_IDLE) : busy-poll UdpReceiver → SpscSink
//   hot_thread (core 2, SCHED_FIFO) : drain SpscSink → FeedHandler → pipeline
//                                     → SimpleQuoter → RiskGateway → OE wire
//                                     + poll_reports() on the same hot loop
//
// Usage / 用法:
//   hft <md_addr> <md_port> <oe_host> <oe_port> [mcast_group] [--fix]
//
//   md_addr    : local address to bind for UDP market data ("0.0.0.0")
//   md_port    : UDP port for market data
//   oe_host    : exchange OE TCP host
//   oe_port    : exchange OE TCP port
//   mcast_group: optional multicast group (e.g. "239.1.1.1")
//   --fix      : use FIX ASCII parser instead of binary parser
//
// Example / 示例:
//   hft 0.0.0.0 12345 127.0.0.1 9001            # binary feed (default)
//   hft 0.0.0.0 12346 127.0.0.1 9001 --fix      # FIX feed
//   hft 0.0.0.0 12345 127.0.0.1 9001 239.1.1.1  # binary + multicast

#include <hft/core/map_order_book.hpp>
#include <hft/core/thread_util.hpp>
#include <hft/core/types.hpp>
#include <hft/exchange/byte_sink.hpp>
#include <hft/exchange/spsc_sink.hpp>
#include <hft/md/binary_parser.hpp>
#include <hft/md/fix_parser.hpp>
#include <hft/net/udp_receiver.hpp>
#include <hft/oe/order_gateway.hpp>
#include <hft/oe/tcp_session.hpp>
#include <hft/pipeline/book_builder.hpp>
#include <hft/pipeline/feed_handler.hpp>
#include <hft/pipeline/gap_detector.hpp>
#include <hft/risk/risk_gateway.hpp>
#include <hft/risk/risk_limits.hpp>
#include <hft/strategy/i_order_gateway.hpp>
#include <hft/strategy/i_order_listener.hpp>
#include <hft/strategy/simple_quoter.hpp>

#include <atomic>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>

using namespace hft;

// ── Shutdown flag (set by SIGINT / SIGTERM) ───────────────────────────────────
// 停机标志（由 SIGINT / SIGTERM 设置）
static std::atomic<bool> g_stop{false};
static void on_signal(int) noexcept {
  g_stop.store(true, std::memory_order_relaxed);
}

// ── DeferredGateway ───────────────────────────────────────────────────────────
// Breaks the circular construction dependency between strategy and gateway chain.
// 解决策略与网关链之间的循环构造依赖。
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

// ── DiagListener ──────────────────────────────────────────────────────────────
// Minimal IOrderListener: prints execution reports to stderr.
// 最简 IOrderListener：将回报打印到 stderr。
struct DiagListener final : strategy::IOrderListener {
  void on_ack(OrderId clord_id, OrderId exch_id) override {
    fprintf(stderr, "[ACK]    clord=%-6lu  exch=%lu\n", clord_id, exch_id);
  }
  void on_fill(OrderId exch_id, Qty filled_qty, Price px) override {
    fprintf(stderr, "[FILL]   exch=%-7lu  qty=%-6ld  px=%ld\n",
            exch_id, filled_qty, px);
  }
  void on_cancel_ack(OrderId exch_id) override {
    fprintf(stderr, "[CXLACK] exch=%lu\n", exch_id);
  }
  void on_reject(OrderId clord_id, std::string_view reason) override {
    fprintf(stderr, "[REJECT] clord=%-6lu  reason=%.*s\n",
            clord_id, static_cast<int>(reason.size()), reason.data());
  }
};

// ── Pipeline template ─────────────────────────────────────────────────────────
// Instantiated twice (BinaryParser + FixParser) but only one branch runs.
// 实例化两次（BinaryParser + FixParser），但运行时只走一条分支。
template <typename Parser>
static int run(const std::string &md_addr, uint16_t md_port,
               const std::string &oe_host, uint16_t oe_port,
               const std::string &mcast) {
  constexpr md::SymbolId kSym     = 1;
  constexpr Qty          kOrderQty = 1;
  constexpr int          kIoCore   = 1;
  constexpr int          kHotCore  = 2;

  risk::RiskLimits limits{};
  limits.max_order_qty        = 1'000;
  limits.max_order_notional   = 100'000 * kPriceScale;
  limits.max_orders_per_sec   = 200;
  limits.fat_finger_bps       = 500.0;
  limits.max_position_per_sym = 10'000;

  DiagListener oe_listener;
  oe::TcpSession oe_session(oe_host, oe_port);
  oe::OrderGatewayT<oe::TcpSession> gw(oe_listener, oe_session);
  risk::RiskGateway risk_gw(gw, limits);

  DeferredGateway deferred_gw;
  strategy::SimpleQuoter quoter(deferred_gw, {kSym, kOrderQty});

  using SBook = core::MapOrderBook<strategy::SimpleQuoter>;
  pipeline::BookBuilder<pipeline::NullNext, SBook> bb(
      [&](md::SymbolId sym) { return SBook(quoter, sym); });

  pipeline::GapDetector<decltype(bb)> gd(
      bb,
      [](uint64_t from, uint64_t to) {
        fprintf(stderr, "[GAP]  seq [%lu, %lu] — no retransmit channel\n",
                from, to);
      },
      [] { fprintf(stderr, "[OVERFLOW] reorder buffer full\n"); });

  pipeline::FeedHandler<decltype(gd), Parser> fh(gd);

  auto spsc = std::make_unique<exchange::SpscSink<1024>>();

  net::UdpReceiver md_rx(md_addr, md_port);
  if (!md_rx.ok()) {
    fprintf(stderr, "ERROR: failed to bind UDP %s:%u\n",
            md_addr.c_str(), md_port);
    return 1;
  }
  if (!mcast.empty() && !md_rx.join_multicast(mcast))
    fprintf(stderr, "WARN:  failed to join multicast group %s\n", mcast.c_str());

  deferred_gw.bind(risk_gw);

  if (!oe_session.connect()) {
    fprintf(stderr, "ERROR: failed to connect OE %s:%u\n",
            oe_host.c_str(), oe_port);
    return 1;
  }
  fprintf(stderr, "INFO:  OE connected to %s:%u\n", oe_host.c_str(), oe_port);
  fprintf(stderr, "INFO:  MD listening on %s:%u%s%s  [%s]\n",
          md_addr.c_str(), md_port,
          mcast.empty() ? "" : " mcast=", mcast.c_str(),
          std::is_same_v<Parser, md::FixParser> ? "FIX" : "binary");

  std::thread io_thread([&] {
    core::pin_to_core(kIoCore);
    core::set_idle_priority();
    while (!g_stop.load(std::memory_order_relaxed))
      md_rx.recv_batch(*spsc);
  });

  core::pin_to_core(kHotCore);
  core::set_realtime_priority();

  using Pkt = exchange::SpscSink<1024>::PacketT;
  Pkt pkt;

  while (!g_stop.load(std::memory_order_relaxed)) {
    while (spsc->try_pop(pkt))
      fh.on_bytes(pkt.data.data(), pkt.len);
    gw.poll_reports();
  }

  io_thread.join();

  fprintf(stderr,
          "\n--- shutdown stats ---\n"
          "  md  msgs_parsed  : %lu\n"
          "  md  corrupt      : %lu\n"
          "  spsc dropped     : %lu\n"
          "  risk rejected    : kill=%lu qty=%lu notional=%lu "
          "rate=%lu fat_finger=%lu position=%lu\n",
          fh.msgs_parsed(),
          fh.corrupt_events(),
          spsc->dropped(),
          risk_gw.rejected_kill(),
          risk_gw.rejected_qty(),
          risk_gw.rejected_notional(),
          risk_gw.rejected_rate(),
          risk_gw.rejected_fat_finger(),
          risk_gw.rejected_position());

  return 0;
}

// ── main ──────────────────────────────────────────────────────────────────────
int main(int argc, char **argv) {
  if (argc < 5) {
    fprintf(stderr,
            "Usage: hft <md_addr> <md_port> <oe_host> <oe_port>"
            " [mcast_group] [--fix]\n"
            "  md_addr    : UDP bind address (e.g. 0.0.0.0)\n"
            "  md_port    : UDP market-data port\n"
            "  oe_host    : OE TCP host\n"
            "  oe_port    : OE TCP port\n"
            "  mcast_group: optional multicast group (e.g. 239.1.1.1)\n"
            "  --fix      : use FIX ASCII parser (default: binary)\n");
    return 1;
  }

  const std::string md_addr = argv[1];
  const auto md_port        = static_cast<uint16_t>(std::atoi(argv[2]));
  const std::string oe_host = argv[3];
  const auto oe_port        = static_cast<uint16_t>(std::atoi(argv[4]));

  std::string mcast;
  bool use_fix = false;
  for (int i = 5; i < argc; ++i) {
    if (std::string_view(argv[i]) == "--fix")
      use_fix = true;
    else
      mcast = argv[i];
  }

  std::signal(SIGINT,  on_signal);
  std::signal(SIGTERM, on_signal);

  if (use_fix)
    return run<md::FixParser>(md_addr, md_port, oe_host, oe_port, mcast);
  else
    return run<md::BinaryParser>(md_addr, md_port, oe_host, oe_port, mcast);
}
