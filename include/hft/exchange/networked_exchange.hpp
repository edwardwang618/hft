// include/hft/exchange/networked_exchange.hpp
// Scripted exchange: broadcasts MD on two UDP ports (binary + FIX),
// accepts one OE TCP client, and runs a data-driven scenario.
// 脚本化交易所：通过两个 UDP 端口广播 MD（binary + FIX），
// 接受一条 OE TCP 连接，执行数据驱动的场景。
//
// MD events are loaded from a CSV file (see csv_md_player.hpp) or from
// the built-in 8-second scenario when no file is given.
// MD 事件从 CSV 文件加载（参见 csv_md_player.hpp），未指定文件时使用内置场景。
//
// Matching / 撮合:
//   Every NewOrder → immediate Ack, then cross-check against internal book:
//   每笔 NewOrder → 立即 Ack，然后与内部订单簿交叉撮合：
//     Buy  @ px ≥ best_ask → Fill + Exec MD + Trade MD
//     Sell @ px ≤ best_bid → Fill + Exec MD + Trade MD
#pragma once

#include <hft/core/types.hpp>
#include <hft/exchange/binary_serializer.hpp>
#include <hft/exchange/csv_md_player.hpp>
#include <hft/exchange/fix_serializer.hpp>
#include <hft/md/md_event.hpp>
#include <hft/oe/wire_oe.hpp>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

namespace hft::exchange {

class NetworkedExchange {
public:
  struct Config {
    std::string  md_dest{"127.0.0.1"};  // UDP destination (both feeds)
    uint16_t     binary_port{12345};     // binary MD UDP port
    uint16_t     fix_port{12346};        // FIX MD UDP port; 0 = disabled
    std::string  oe_addr{"0.0.0.0"};    // OE TCP listen address
    uint16_t     oe_port{9001};
    md::SymbolId sym{1};
    std::string  scenario_csv;           // path to CSV; empty = built-in scenario
                                         // CSV 路径；为空时使用内置场景
  };

  explicit NetworkedExchange(Config cfg) noexcept : cfg_(std::move(cfg)) {}
  ~NetworkedExchange() { cleanup(); }

  NetworkedExchange(const NetworkedExchange &) = delete;
  NetworkedExchange &operator=(const NetworkedExchange &) = delete;

  bool run() {
    if (!setup()) return false;
    const bool ok = run_loop();
    cleanup();
    return ok;
  }

private:
  // Single-level resting order per side (best bid / best ask).
  // 每方向的最优单档挂单（最优买价 / 最优卖价）。
  struct BookLevel {
    uint64_t exch_id{0};
    Price    px{0};
    Qty      qty{0};
    bool     valid{false};
  };

  Config           cfg_;
  int              udp_fd_{-1};
  int              listen_fd_{-1};
  int              client_fd_{-1};

  BinarySerializer bin_ser_;
  FixSerializer    fix_ser_;
  uint64_t         next_seq_{1};

  sockaddr_in      binary_addr_{};
  sockaddr_in      fix_addr_{};

  std::array<std::byte, 256> buf_{};

  // ── Setup / cleanup ────────────────────────────────────────────────────────

  bool setup() noexcept {
    udp_fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (udp_fd_ < 0) { perror("socket(UDP)"); return false; }
    const int ttl = 1;
    ::setsockopt(udp_fd_, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));

    binary_addr_ = make_addr(cfg_.md_dest, cfg_.binary_port);
    if (cfg_.fix_port > 0)
      fix_addr_ = make_addr(cfg_.md_dest, cfg_.fix_port);

    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) { perror("socket(TCP)"); return false; }
    int yes = 1;
    ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEPORT, &yes, sizeof(yes));

    sockaddr_in oe_bind = make_addr(cfg_.oe_addr, cfg_.oe_port);
    if (::bind(listen_fd_,
               reinterpret_cast<sockaddr *>(&oe_bind), sizeof(oe_bind)) < 0) {
      perror("bind(OE)");
      return false;
    }
    ::listen(listen_fd_, 1);

    fprintf(stderr, "INFO:  MD(binary) → %s:%u\n",
            cfg_.md_dest.c_str(), cfg_.binary_port);
    if (cfg_.fix_port > 0)
      fprintf(stderr, "INFO:  MD(FIX)    → %s:%u\n",
              cfg_.md_dest.c_str(), cfg_.fix_port);
    fprintf(stderr, "INFO:  OE listen   %s:%u\n",
            cfg_.oe_addr.c_str(), cfg_.oe_port);
    return true;
  }

  void cleanup() noexcept {
    if (client_fd_ >= 0) { ::close(client_fd_); client_fd_ = -1; }
    if (listen_fd_ >= 0) { ::close(listen_fd_); listen_fd_ = -1; }
    if (udp_fd_    >= 0) { ::close(udp_fd_);    udp_fd_    = -1; }
  }

  static sockaddr_in make_addr(const std::string &host,
                                uint16_t port) noexcept {
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port   = htons(port);
    ::inet_pton(AF_INET, host.c_str(), &a.sin_addr);
    return a;
  }

  // ── Built-in scenario (hardcoded fallback) ─────────────────────────────────
  // Replicates the default.csv scenario so the binary works without any file.
  // 复制 default.csv 场景，使二进制在没有文件的情况下也能运行。

  std::vector<TimedEvent> builtin_scenario() const noexcept {
    const md::SymbolId s    = cfg_.sym;
    constexpr Qty   kQty  = 1000;
    constexpr Price kAsk0 = 101 * kPriceScale;
    constexpr Price kBid0 = 99  * kPriceScale;
    constexpr Price kAsk1 = 100 * kPriceScale;
    constexpr Price kBid1 = 100 * kPriceScale;
    return {
      {   0, md::Add{0, s, 1, Side::Sell, kAsk0, kQty}},
      {   0, md::Add{0, s, 2, Side::Buy,  kBid0, kQty}},
      {2000, md::Cancel{0, s, 1}},
      {2000, md::Add{0, s, 3, Side::Sell, kAsk1, kQty}},
      {4000, md::Cancel{0, s, 2}},
      {4000, md::Add{0, s, 4, Side::Buy,  kBid1, kQty}},
    };
  }

  // ── MD broadcast ──────────────────────────────────────────────────────────

  void broadcast(const md::MdEvent &ev) noexcept {
    std::size_t n = bin_ser_.encode(ev, next_seq_, buf_.data(), buf_.size());
    if (n > 0)
      ::sendto(udp_fd_, buf_.data(), n, 0,
               reinterpret_cast<const sockaddr *>(&binary_addr_),
               sizeof(binary_addr_));

    if (cfg_.fix_port > 0) {
      n = fix_ser_.encode(ev, next_seq_, buf_.data(), buf_.size());
      if (n > 0)
        ::sendto(udp_fd_, buf_.data(), n, 0,
                 reinterpret_cast<const sockaddr *>(&fix_addr_),
                 sizeof(fix_addr_));
    }

    ++next_seq_;
  }

  // ── Internal book update ───────────────────────────────────────────────────
  // Keeps best_ask / best_bid in sync with broadcast events so the matching
  // engine reflects what was published.
  // 将 best_ask / best_bid 与广播事件同步，使撮合引擎反映已发布的状态。

  static void apply_to_book(BookLevel &ask, BookLevel &bid,
                              const md::MdEvent &ev) noexcept {
    std::visit([&](const auto &e) {
      using T = std::decay_t<decltype(e)>;
      if constexpr (std::is_same_v<T, md::Add>) {
        if (e.side == Side::Sell && (!ask.valid || e.px <= ask.px))
          ask = {e.id, e.px, e.qty, true};
        else if (e.side == Side::Buy && (!bid.valid || e.px >= bid.px))
          bid = {e.id, e.px, e.qty, true};
      } else if constexpr (std::is_same_v<T, md::Cancel>) {
        if (ask.valid && ask.exch_id == e.id) ask = {};
        if (bid.valid && bid.exch_id == e.id) bid = {};
      } else if constexpr (std::is_same_v<T, md::Reduce>) {
        if (ask.valid && ask.exch_id == e.id) ask.qty = e.new_qty;
        if (bid.valid && bid.exch_id == e.id) bid.qty = e.new_qty;
      } else if constexpr (std::is_same_v<T, md::Clear>) {
        ask = {}; bid = {};
      }
    }, ev);
  }

  // ── OE helpers ────────────────────────────────────────────────────────────

  static bool tcp_send(int fd, const void *data, std::size_t len) noexcept {
    const auto *p = static_cast<const char *>(data);
    while (len > 0) {
      const ssize_t n = ::send(fd, p, len, MSG_NOSIGNAL);
      if (n <= 0) return false;
      p   += n;
      len -= static_cast<std::size_t>(n);
    }
    return true;
  }

  void send_ack(md::SymbolId sym, OrderId clord_id,
                OrderId exch_id) noexcept {
    oe::wire::Ack ack{};
    ack.hdr.msg_type = oe::wire::kAck;
    ack.hdr.msg_len  = sizeof(oe::wire::Ack);
    ack.hdr.clord_id = clord_id;
    ack.hdr.sym      = sym;
    ack.exch_id      = exch_id;
    if (!tcp_send(client_fd_, &ack, sizeof(ack)))
      fprintf(stderr, "WARN:  send_ack failed\n");
    else
      fprintf(stderr, "[ACK ]  clord=%-6lu  exch=%lu\n", clord_id, exch_id);
  }

  void send_fill(md::SymbolId sym, OrderId clord_id, OrderId exch_id,
                 Qty qty, Price px) noexcept {
    oe::wire::Fill fill{};
    fill.hdr.msg_type = oe::wire::kFill;
    fill.hdr.msg_len  = sizeof(oe::wire::Fill);
    fill.hdr.clord_id = clord_id;
    fill.hdr.sym      = sym;
    fill.exch_id      = exch_id;
    fill.filled_qty   = qty;
    fill.px           = px;
    if (!tcp_send(client_fd_, &fill, sizeof(fill)))
      fprintf(stderr, "WARN:  send_fill failed\n");
    else
      fprintf(stderr, "[FILL]  clord=%-6lu  exch=%-6lu  qty=%-6ld  px=%ld\n",
              clord_id, exch_id, qty, px);
  }

  // ── OE frame processing ────────────────────────────────────────────────────

  void handle_new_order(const oe::wire::NewOrder &msg,
                        BookLevel &ask, BookLevel &bid,
                        uint64_t &next_exch_id, uint64_t &next_trade_id,
                        std::size_t &orders_received,
                        std::size_t &fills_sent) noexcept {
    const OrderId      clord_id = msg.hdr.clord_id;
    const OrderId      exch_id  = next_exch_id++;
    const md::SymbolId sym      = msg.hdr.sym;
    const Side         side     = static_cast<Side>(msg.side);
    const Price        px       = msg.px;
    const Qty          qty      = msg.qty;
    ++orders_received;

    fprintf(stderr, "[NEW ]  clord=%-6lu  sym=%-4u  %s  px=%.4f  qty=%ld\n",
            clord_id, sym,
            side == Side::Buy ? "BUY " : "SELL",
            static_cast<double>(px) / kPriceScale, qty);
    log_book(ask, bid);

    send_ack(sym, clord_id, exch_id);

    bool crossed = false;
    if (side == Side::Buy && ask.valid && px >= ask.px) {
      const Qty      fqty = std::min(qty, ask.qty);
      const Price    fpx  = ask.px;
      const uint64_t tid  = next_trade_id++;
      ask.qty -= fqty;
      if (ask.qty == 0) ask = {};
      broadcast(md::Exec{0, sym, ask.exch_id, fqty, fpx, tid});
      broadcast(md::Trade{0, sym, fpx, fqty, tid});
      send_fill(sym, clord_id, exch_id, fqty, fpx);
      ++fills_sent;
      crossed = true;
    }
    if (!crossed && side == Side::Sell && bid.valid && px <= bid.px) {
      const Qty      fqty = std::min(qty, bid.qty);
      const Price    fpx  = bid.px;
      const uint64_t tid  = next_trade_id++;
      bid.qty -= fqty;
      if (bid.qty == 0) bid = {};
      broadcast(md::Exec{0, sym, bid.exch_id, fqty, fpx, tid});
      broadcast(md::Trade{0, sym, fpx, fqty, tid});
      send_fill(sym, clord_id, exch_id, fqty, fpx);
      ++fills_sent;
    }
  }

  // Drains the OE receive buffer and processes complete frames.
  // 排尽 OE 接收缓冲区并处理完整帧。
  // Returns false if the client disconnected or a socket error occurred.
  bool drain_oe(std::vector<std::byte> &oe_buf,
                std::array<std::byte, 4096> &tmp,
                BookLevel &ask, BookLevel &bid,
                uint64_t &next_exch_id, uint64_t &next_trade_id,
                std::size_t &orders_received,
                std::size_t &fills_sent) noexcept {
    const ssize_t n = ::recv(client_fd_, tmp.data(), tmp.size(), MSG_DONTWAIT);
    if (n > 0) {
      oe_buf.insert(oe_buf.end(), tmp.data(), tmp.data() + n);
    } else if (n == 0) {
      fprintf(stderr, "INFO:  client disconnected\n");
      return false;
    } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
      perror("recv(OE)");
      return false;
    }

    std::size_t offset = 0;
    while (oe_buf.size() - offset >= sizeof(oe::wire::Header)) {
      oe::wire::Header hdr{};
      std::memcpy(&hdr, oe_buf.data() + offset, sizeof(hdr));

      if (hdr.msg_len < sizeof(oe::wire::Header) || hdr.msg_len > 256) {
        fprintf(stderr, "WARN:  corrupt OE frame (msg_len=%u) — resetting\n",
                hdr.msg_len);
        oe_buf.clear();
        return true;
      }
      if (oe_buf.size() - offset < hdr.msg_len)
        break;

      if (hdr.msg_type == oe::wire::kNewOrder &&
          hdr.msg_len  >= sizeof(oe::wire::NewOrder)) {
        oe::wire::NewOrder msg{};
        std::memcpy(&msg, oe_buf.data() + offset, sizeof(msg));
        handle_new_order(msg, ask, bid,
                         next_exch_id, next_trade_id,
                         orders_received, fills_sent);
      }

      offset += hdr.msg_len;
    }
    if (offset > 0)
      oe_buf.erase(oe_buf.begin(),
                   oe_buf.begin() + static_cast<std::ptrdiff_t>(offset));
    return true;
  }

  // ── Main scenario loop ─────────────────────────────────────────────────────

  bool run_loop() {
    // Load scenario from CSV or fall back to the built-in sequence.
    // 从 CSV 加载场景，或回退到内置序列。
    std::vector<TimedEvent> events;
    if (cfg_.scenario_csv.empty()) {
      events = builtin_scenario();
      fprintf(stderr, "INFO:  using built-in scenario (%zu events)\n",
              events.size());
    } else {
      events = CsvMdPlayer::load(cfg_.scenario_csv);
      if (events.empty()) return false;
    }

    // Run until 2 s after the last event (minimum 8 s for the built-in demo).
    // 场景结束时间：最后事件后 2 秒（内置场景至少 8 秒）。
    const uint32_t end_ms =
        events.empty() ? 8000u
                       : std::max(8000u, events.back().delay_ms + 2000u);

    // Wait for the OE client to connect before broadcasting anything.
    // This guarantees the client's UDP socket is bound before we send.
    // 在广播任何内容前等待 OE 客户端连接，确保客户端 UDP socket 已绑定。
    fprintf(stderr, "INFO:  waiting for OE connection...\n");
    client_fd_ = ::accept(listen_fd_, nullptr, nullptr);
    if (client_fd_ < 0) { perror("accept"); return false; }
    int yes = 1;
    ::setsockopt(client_fd_, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
    fprintf(stderr, "INFO:  client connected — starting scenario\n");

    BookLevel resting_ask{}, resting_bid{};
    uint64_t  next_exch_id{1}, next_trade_id{1};
    std::size_t orders_received{0}, fills_sent{0};
    std::size_t next_ev{0};

    std::vector<std::byte>      oe_buf;
    std::array<std::byte, 4096> tmp{};

    using clock = std::chrono::steady_clock;
    using ms    = std::chrono::milliseconds;
    const auto t0  = clock::now();
    auto now_ms    = [&]() -> uint32_t {
      return static_cast<uint32_t>(
          std::chrono::duration_cast<ms>(clock::now() - t0).count());
    };

    bool running = true;
    while (running) {
      const uint32_t t = now_ms();

      // Dispatch all events whose time has arrived.
      // 发出所有到达时间的事件。
      while (next_ev < events.size() && events[next_ev].delay_ms <= t) {
        const md::MdEvent &ev = events[next_ev].ev;
        apply_to_book(resting_ask, resting_bid, ev);
        broadcast(ev);
        log_event(t, ev);
        ++next_ev;
      }

      if (t >= end_ms) {
        fprintf(stderr, "INFO:  [T+%ums] scenario complete — shutting down\n", t);
        break;
      }

      if (!drain_oe(oe_buf, tmp, resting_ask, resting_bid,
                    next_exch_id, next_trade_id,
                    orders_received, fills_sent)) {
        running = false;
      }

      std::this_thread::sleep_for(std::chrono::microseconds(200));
    }

    fprintf(stderr,
            "\n--- exchange stats ---\n"
            "  orders received : %zu\n"
            "  fills sent      : %zu\n"
            "  MD frames sent  : %lu\n",
            orders_received, fills_sent, next_seq_ - 1);
    return true;
  }

  // ── Logging ───────────────────────────────────────────────────────────────

  static void log_book(const BookLevel &ask, const BookLevel &bid) noexcept {
    auto fmt = [](const BookLevel &lvl, const char *side) {
      if (lvl.valid)
        fprintf(stderr, "       %-4s  id=%-4lu  px=%.4f  qty=%ld\n",
                side, lvl.exch_id,
                static_cast<double>(lvl.px) / kPriceScale, lvl.qty);
      else
        fprintf(stderr, "       %-4s  —\n", side);
    };
    fprintf(stderr, "     [book]\n");
    fmt(ask, "ask");
    fmt(bid, "bid");
  }

  static void log_event(uint32_t t_ms, const md::MdEvent &ev) noexcept {
    std::visit([&](const auto &e) {
      using T = std::decay_t<decltype(e)>;
      if constexpr (std::is_same_v<T, md::Add>) {
        fprintf(stderr,
                "INFO:  [T+%4ums] broadcast Add    id=%-4lu  %s  px=%.4f  qty=%ld\n",
                t_ms, e.id,
                e.side == Side::Buy ? "BUY " : "SELL",
                static_cast<double>(e.px) / kPriceScale, e.qty);
      } else if constexpr (std::is_same_v<T, md::Cancel>) {
        fprintf(stderr,
                "INFO:  [T+%4ums] broadcast Cancel id=%lu\n", t_ms, e.id);
      } else if constexpr (std::is_same_v<T, md::Reduce>) {
        fprintf(stderr,
                "INFO:  [T+%4ums] broadcast Reduce id=%-4lu  new_qty=%ld\n",
                t_ms, e.id, e.new_qty);
      } else if constexpr (std::is_same_v<T, md::Clear>) {
        fprintf(stderr, "INFO:  [T+%4ums] broadcast Clear\n", t_ms);
      }
    }, ev);
  }
};

} // namespace hft::exchange
