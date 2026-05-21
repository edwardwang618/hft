#include <hft/exchange/spsc_sink.hpp>
#include <hft/net/udp_receiver.hpp>

#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <cstring>
#include <string>
#include <vector>

using namespace hft;
using namespace hft::net;
using namespace hft::exchange;

// ── helper: open a plain UDP sender socket ───────────────────────────────────

static int make_sender() { return ::socket(AF_INET, SOCK_DGRAM, 0); }

static void send_to(int fd, uint16_t port, const void *data, std::size_t len) {
  struct sockaddr_in dst{};
  dst.sin_family = AF_INET;
  dst.sin_port = htons(port);
  ::inet_pton(AF_INET, "127.0.0.1", &dst.sin_addr);
  ::sendto(fd, data, len, 0, reinterpret_cast<const struct sockaddr *>(&dst),
           sizeof(dst));
}

// ── collecting sink ──────────────────────────────────────────────────────────

struct CollectSink : IByteSink {
  void on_bytes(const std::byte *data, std::size_t n) noexcept override {
    packets.emplace_back(reinterpret_cast<const char *>(data),
                         reinterpret_cast<const char *>(data) + n);
  }
  std::vector<std::string> packets;
};

// ── tests
// ─────────────────────────────────────────────────────────────────────

TEST(UdpReceiver, BindsSuccessfully) {
  UdpReceiver rx("127.0.0.1", 0);
  EXPECT_TRUE(rx.ok());
  EXPECT_GT(rx.actual_port(), 0);
}

TEST(UdpReceiver, InvalidAddressFails) {
  UdpReceiver rx("not.an.ip", 0);
  EXPECT_FALSE(rx.ok());
}

TEST(UdpReceiver, EmptyRecvReturnsZero) {
  UdpReceiver rx("127.0.0.1", 0);
  ASSERT_TRUE(rx.ok());

  CollectSink sink;
  EXPECT_EQ(rx.recv_batch(sink), 0);
  EXPECT_TRUE(sink.packets.empty());
}

TEST(UdpReceiver, ReceiveSinglePacket) {
  UdpReceiver rx("127.0.0.1", 0);
  ASSERT_TRUE(rx.ok());

  const uint16_t port = rx.actual_port();
  int sender = make_sender();
  ASSERT_GE(sender, 0);

  const std::string msg = "hello";
  send_to(sender, port, msg.data(), msg.size());
  ::close(sender);

  // Brief spin to let the kernel deliver the packet.
  CollectSink sink;
  for (int tries = 0; tries < 100 && sink.packets.empty(); ++tries)
    rx.recv_batch(sink);

  ASSERT_EQ(sink.packets.size(), 1u);
  EXPECT_EQ(sink.packets[0], msg);
}

TEST(UdpReceiver, BatchReceiveMultiplePackets) {
  UdpReceiver rx("127.0.0.1", 0);
  ASSERT_TRUE(rx.ok());

  const uint16_t port = rx.actual_port();
  int sender = make_sender();
  ASSERT_GE(sender, 0);

  const int kCount = 10;
  for (int i = 0; i < kCount; ++i) {
    const std::string msg = "pkt" + std::to_string(i);
    send_to(sender, port, msg.data(), msg.size());
  }
  ::close(sender);

  CollectSink sink;
  for (int tries = 0; tries < 200 && (int)sink.packets.size() < kCount; ++tries)
    rx.recv_batch(sink);

  ASSERT_EQ((int)sink.packets.size(), kCount);
  for (int i = 0; i < kCount; ++i)
    EXPECT_EQ(sink.packets[i], "pkt" + std::to_string(i));
}

TEST(UdpReceiver, SpscSinkIntegration) {
  UdpReceiver rx("127.0.0.1", 0);
  ASSERT_TRUE(rx.ok());

  const uint16_t port = rx.actual_port();
  int sender = make_sender();
  ASSERT_GE(sender, 0);

  SpscSink<64> sink;

  const std::string payload = "spsc_test";
  send_to(sender, port, payload.data(), payload.size());
  ::close(sender);

  for (int tries = 0; tries < 100; ++tries) {
    rx.recv_batch(sink);
    core::Packet<2048> pkt;
    if (sink.try_pop(pkt)) {
      EXPECT_EQ(pkt.len, payload.size());
      EXPECT_EQ(std::memcmp(pkt.data.data(), payload.data(), payload.size()),
                0);
      return;
    }
  }
  FAIL() << "packet never arrived";
}