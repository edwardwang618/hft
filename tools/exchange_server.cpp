// tools/exchange_server.cpp
// Scripted exchange server for end-to-end testing against real sockets.
// 用于端到端测试的脚本化交易所服务器（真实 socket）。
//
// Broadcasts market data on two UDP ports (binary + FIX) simultaneously
// and accepts one OE TCP connection.  See networked_exchange.hpp for the
// full scenario description.
// 同时在两个 UDP 端口（binary + FIX）广播行情，并接受一条 OE TCP 连接。
// 完整场景说明见 networked_exchange.hpp。
//
// Usage / 用法:
//   exchange_server <md_dest> <binary_port> <fix_port> <oe_addr> <oe_port>
//
//   md_dest     : UDP destination (unicast or multicast)
//   binary_port : binary MD UDP port
//   fix_port    : FIX MD UDP port  (0 = disabled)
//   oe_addr     : OE TCP listen address (e.g. 0.0.0.0)
//   oe_port     : OE TCP port
//
// Example / 示例 (pair with hft binaries below):
//   exchange_server 127.0.0.1 12345 12346 0.0.0.0 9001
//   hft 0.0.0.0 12345 127.0.0.1 9001          # binary feed
//   hft 0.0.0.0 12346 127.0.0.1 9001 --fix    # FIX feed

#include <hft/exchange/networked_exchange.hpp>

#include <cstdint>
#include <cstdio>
#include <cstdlib>

int main(int argc, char **argv) {
  if (argc < 6) {
    fprintf(stderr,
            "Usage: exchange_server <md_dest> <binary_port> <fix_port>"
            " <oe_addr> <oe_port>\n"
            "  md_dest     : UDP destination (unicast or multicast)\n"
            "  binary_port : binary MD UDP port\n"
            "  fix_port    : FIX MD UDP port (0 = disabled)\n"
            "  oe_addr     : OE TCP listen address (e.g. 0.0.0.0)\n"
            "  oe_port     : OE TCP port\n");
    return 1;
  }

  hft::exchange::NetworkedExchange::Config cfg;
  cfg.md_dest     = argv[1];
  cfg.binary_port = static_cast<uint16_t>(std::atoi(argv[2]));
  cfg.fix_port    = static_cast<uint16_t>(std::atoi(argv[3]));
  cfg.oe_addr     = argv[4];
  cfg.oe_port     = static_cast<uint16_t>(std::atoi(argv[5]));

  return hft::exchange::NetworkedExchange(cfg).run() ? 0 : 1;
}
