// include/hft/exchange/networked_exchange.hpp
// Scripted exchange: broadcasts MD on two UDP ports (binary + FIX),
// accepts one OE TCP client, and runs a deterministic 8-second scenario.
// 脚本化交易所：通过两个 UDP 端口广播 MD（binary + FIX），
// 接受一条 OE TCP 连接，执行确定性的 8 秒脚本场景。
//
// Scenario timeline / 场景时间线:
//   T+0s : Broadcast Add(Sell@101) + Add(Buy@99)  — seed book
//   T+2s : Cancel ask@101 → Add ask@100           — ask tightens
//   T+4s : Cancel bid@99  → Add bid@100           — bid tightens (locked)
//   T+8s : Shutdown
//
// Every NewOrder received → immediate Ack, then cross-check:
//   Buy  @ px ≥ best_ask → Fill + Exec MD + Trade MD
//   Sell @ px ≤ best_bid → Fill + Exec MD + Trade MD
#pragma once

#include <hft/core/types.hpp>
#include <hft/exchange/binary_serializer.hpp>
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
#include <vector>

namespace hft::exchange {

class NetworkedExchange {
public:
  struct Config {
    std::string md_dest{"127.0.0.1"}; // UDP destination (both feeds)
    uint16_t binary_port{12345};      // binary MD UDP port
    uint16_t fix_port{12346};         // FIX MD UDP port; 0 = disabled
    std::string oe_addr{"0.0.0.0"};   // OE TCP listen address
    uint16_t oe_port{9001};
    md::SymbolId sym{1};
  };

  explicit NetworkedExchange(Config cfg) noexcept : cfg_(std::move(cfg)) {}
  ~NetworkedExchange() { cleanup(); }

  NetworkedExchange(const NetworkedExchange &) = delete;
  NetworkedExchange &operator=(const NetworkedExchange &) = delete;

  // Runs the scripted scenario. Blocks until complete or error. Returns false
  // on error. 执行脚本场景，阻塞直到结束或出错。出错返回 false。
  bool run() {
    if (!setup())
      return false;
    const bool ok = run_loop();
    cleanup();
    return ok;
  }

private:
  // Single-level resting order per side.
  // 每方向一笔挂单的单档位订单簿。
  struct BookLevel {
    uint64_t exch_id{0};
    Price px{0};
    Qty qty{0};
    bool valid{false};
  };

  Config cfg_;
  int udp_fd_{-1};
  int listen_fd_{-1};
  int client_fd_{-1};

  BinarySerializer bin_ser_;
  FixSerializer fix_ser_;
  uint64_t next_seq_{1};

  sockaddr_in binary_addr_{};
  sockaddr_in fix_addr_{};

  std::array<std::byte, 256> buf_{};

  // ── Setup / cleanup ────────────────────────────────────────────────────────

  bool setup() noexcept {
    udp_fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (udp_fd_ < 0) {
      perror("socket(UDP)");
      return false;
    }

    const int ttl = 1;
    ::setsockopt(udp_fd_, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));

    binary_addr_ = make_addr(cfg_.md_dest, cfg_.binary_port);
    if (cfg_.fix_port > 0)
      fix_addr_ = make_addr(cfg_.md_dest, cfg_.fix_port);

    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) {
      perror("socket(TCP)");
      return false;
    }

    int yes = 1;
    ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEPORT, &yes, sizeof(yes));

    sockaddr_in oe_bind = make_addr(cfg_.oe_addr, cfg_.oe_port);
    if (::bind(listen_fd_, reinterpret_cast<sockaddr *>(&oe_bind),
               sizeof(oe_bind)) < 0) {
      perror("bind(OE)");
      return false;
    }
    ::listen(listen_fd_, 1);

    fprintf(stderr, "INFO:  MD(binary) → %s:%u\n", cfg_.md_dest.c_str(),
            cfg_.binary_port);
    if (cfg_.fix_port > 0)
      fprintf(stderr, "INFO:  MD(FIX)    → %s:%u\n", cfg_.md_dest.c_str(),
              cfg_.fix_port);
    fprintf(stderr, "INFO:  OE listen   %s:%u\n", cfg_.oe_addr.c_str(),
            cfg_.oe_port);
    return true;
  }

  void cleanup() noexcept {
    if (client_fd_ >= 0) {
      ::close(client_fd_);
      client_fd_ = -1;
    }
    if (listen_fd_ >= 0) {
      ::close(listen_fd_);
      listen_fd_ = -1;
    }
    if (udp_fd_ >= 0) {
      ::close(udp_fd_);
      udp_fd_ = -1;
    }
  }

  static sockaddr_in make_addr(const std::string &host,
                               uint16_t port) noexcept {
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    ::inet_pton(AF_INET, host.c_str(), &a.sin_addr);
    return a;
  }

  // ── MD broadcast ──────────────────────────────────────────────────────────

  // Sends the same logical event on both feeds with the same sequence number.
  // 向两条 feed 以相同序列号广播同一逻辑事件。
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

  // ── OE helpers ────────────────────────────────────────────────────────────

  static bool tcp_send(int fd, const void *data, std::size_t len) noexcept {
    const auto *p = static_cast<const char *>(data);
    while (len > 0) {
      const ssize_t n = ::send(fd, p, len, MSG_NOSIGNAL);
      if (n <= 0)
        return false;
      p += n;
      len -= static_cast<std::size_t>(n);
    }
    return true;
  }

  void send_ack(md::SymbolId sym, OrderId clord_id, OrderId exch_id) noexcept {
    oe::wire::Ack ack{};
    ack.hdr.msg_type = oe::wire::kAck;
    ack.hdr.msg_len = sizeof(oe::wire::Ack);
    ack.hdr.clord_id = clord_id;
    ack.hdr.sym = sym;
    ack.exch_id = exch_id;
    if (!tcp_send(client_fd_, &ack, sizeof(ack)))
      fprintf(stderr, "WARN:  send_ack failed\n");
    else
      fprintf(stderr, "[ACK ]  clord=%-6lu  exch=%lu\n", clord_id, exch_id);
  }

  void send_fill(md::SymbolId sym, OrderId clord_id, OrderId exch_id, Qty qty,
                 Price px) noexcept {
    oe::wire::Fill fill{};
    fill.hdr.msg_type = oe::wire::kFill;
    fill.hdr.msg_len = sizeof(oe::wire::Fill);
    fill.hdr.clord_id = clord_id;
    fill.hdr.sym = sym;
    fill.exch_id = exch_id;
    fill.filled_qty = qty;
    fill.px = px;
    if (!tcp_send(client_fd_, &fill, sizeof(fill)))
      fprintf(stderr, "WARN:  send_fill failed\n");
    else
      fprintf(stderr, "[FILL]  clord=%-6lu  exch=%-6lu  qty=%-6ld  px=%ld\n",
              clord_id, exch_id, qty, px);
  }

  // ── Scripted scenario loop ─────────────────────────────────────────────────

  bool run_loop() {
    constexpr Qty kQty = 1000;
    const md::SymbolId sym = cfg_.sym;
    constexpr Price kAsk0 = 101 * kPriceScale;
    constexpr Price kBid0 = 99 * kPriceScale;
    constexpr Price kAsk1 = 100 * kPriceScale;
    constexpr Price kBid1 = 100 * kPriceScale;
    constexpr uint64_t kIdAsk0 = 1, kIdBid0 = 2, kIdAsk1 = 3, kIdBid1 = 4;

    BookLevel resting_ask{kIdAsk0, kAsk0, kQty, true};
    BookLevel resting_bid{kIdBid0, kBid0, kQty, true};

    broadcast(md::Add{0, sym, kIdAsk0, Side::Sell, kAsk0, kQty});
    broadcast(md::Add{0, sym, kIdBid0, Side::Buy, kBid0, kQty});
    fprintf(stderr, "INFO:  initial book — bid=%.4f  ask=%.4f\n",
            static_cast<double>(kBid0) / kPriceScale,
            static_cast<double>(kAsk0) / kPriceScale);

    fprintf(stderr, "INFO:  waiting for OE connection...\n");
    client_fd_ = ::accept(listen_fd_, nullptr, nullptr);
    if (client_fd_ < 0) {
      perror("accept");
      return false;
    }
    int yes = 1;
    ::setsockopt(client_fd_, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
    fprintf(stderr, "INFO:  client connected\n");

    uint64_t next_exch_id{1}, next_trade_id{1};
    std::size_t orders_received{0}, fills_sent{0};
    bool phase2_done{false}, phase3_done{false}, running{true};

    std::vector<std::byte> oe_buf;
    std::array<std::byte, 4096> tmp_buf;

    using clock = std::chrono::steady_clock;
    using ms = std::chrono::milliseconds;
    const auto start = clock::now();
    auto elapsed_ms = [&]() -> long {
      return std::chrono::duration_cast<ms>(clock::now() - start).count();
    };

    while (running) {
      const long t = elapsed_ms();

      if (!phase2_done && t >= 2000) {
        broadcast(md::Cancel{0, sym, kIdAsk0});
        broadcast(md::Add{0, sym, kIdAsk1, Side::Sell, kAsk1, kQty});
        resting_ask = {kIdAsk1, kAsk1, kQty, true};
        fprintf(stderr, "INFO:  [T+2s] ask tightens → %.4f\n",
                static_cast<double>(kAsk1) / kPriceScale);
        phase2_done = true;
      }

      if (!phase3_done && t >= 4000) {
        broadcast(md::Cancel{0, sym, kIdBid0});
        broadcast(md::Add{0, sym, kIdBid1, Side::Buy, kBid1, kQty});
        resting_bid = {kIdBid1, kBid1, kQty, true};
        fprintf(stderr, "INFO:  [T+4s] bid tightens → %.4f  (locked market)\n",
                static_cast<double>(kBid1) / kPriceScale);
        phase3_done = true;
      }

      if (t >= 8000) {
        fprintf(stderr, "INFO:  [T+8s] scenario complete — shutting down\n");
        break;
      }

      const ssize_t n =
          ::recv(client_fd_, tmp_buf.data(), tmp_buf.size(), MSG_DONTWAIT);
      if (n > 0) {
        oe_buf.insert(oe_buf.end(), tmp_buf.data(), tmp_buf.data() + n);
      } else if (n == 0) {
        fprintf(stderr, "INFO:  client disconnected\n");
        running = false;
        break;
      } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
        perror("recv(OE)");
        running = false;
        break;
      }

      std::size_t offset = 0;
      while (oe_buf.size() - offset >= sizeof(oe::wire::Header)) {
        oe::wire::Header hdr{};
        std::memcpy(&hdr, oe_buf.data() + offset, sizeof(hdr));

        if (hdr.msg_len < sizeof(oe::wire::Header) || hdr.msg_len > 256) {
          fprintf(stderr, "WARN:  corrupt OE frame (msg_len=%u) — resetting\n",
                  hdr.msg_len);
          oe_buf.clear();
          break;
        }
        if (oe_buf.size() - offset < hdr.msg_len)
          break;

        if (hdr.msg_type == oe::wire::kNewOrder &&
            hdr.msg_len >= sizeof(oe::wire::NewOrder)) {
          oe::wire::NewOrder msg{};
          std::memcpy(&msg, oe_buf.data() + offset, sizeof(msg));

          const OrderId clord_id = msg.hdr.clord_id;
          const OrderId exch_id = next_exch_id++;
          const md::SymbolId msym = msg.hdr.sym;
          const Side side = static_cast<Side>(msg.side);
          const Price px = msg.px;
          const Qty qty = msg.qty;
          ++orders_received;

          fprintf(stderr,
                  "[NEW ]  clord=%-6lu  sym=%-4u  %s  px=%.4f  qty=%ld\n",
                  clord_id, msym, side == Side::Buy ? "BUY " : "SELL",
                  static_cast<double>(px) / kPriceScale, qty);

          send_ack(msym, clord_id, exch_id);

          bool crossed = false;
          if (side == Side::Buy && resting_ask.valid && px >= resting_ask.px) {
            const Qty fill_qty = std::min(qty, resting_ask.qty);
            const Price fill_px = resting_ask.px;
            const uint64_t tid = next_trade_id++;
            resting_ask.qty -= fill_qty;
            if (resting_ask.qty == 0)
              resting_ask.valid = false;
            broadcast(
                md::Exec{0, sym, resting_ask.exch_id, fill_qty, fill_px, tid});
            broadcast(md::Trade{0, sym, fill_px, fill_qty, tid});
            send_fill(msym, clord_id, exch_id, fill_qty, fill_px);
            ++fills_sent;
            crossed = true;
          }
          if (!crossed && side == Side::Sell && resting_bid.valid &&
              px <= resting_bid.px) {
            const Qty fill_qty = std::min(qty, resting_bid.qty);
            const Price fill_px = resting_bid.px;
            const uint64_t tid = next_trade_id++;
            resting_bid.qty -= fill_qty;
            if (resting_bid.qty == 0)
              resting_bid.valid = false;
            broadcast(
                md::Exec{0, sym, resting_bid.exch_id, fill_qty, fill_px, tid});
            broadcast(md::Trade{0, sym, fill_px, fill_qty, tid});
            send_fill(msym, clord_id, exch_id, fill_qty, fill_px);
            ++fills_sent;
          }
        }

        offset += hdr.msg_len;
      }

      if (offset > 0)
        oe_buf.erase(oe_buf.begin(),
                     oe_buf.begin() + static_cast<std::ptrdiff_t>(offset));

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
};

} // namespace hft::exchange
