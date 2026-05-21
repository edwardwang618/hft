// include/hft/net/udp_receiver.hpp
// Non-blocking UDP receiver using recvmmsg for batch packet ingestion.
// 使用 recvmmsg 批量收包的非阻塞 UDP 接收器。
//
// Usage (network thread tight loop):
//   UdpReceiver rx("0.0.0.0", 12345);
//   rx.join_multicast("239.1.1.1");
//   while (true) rx.recv_batch(sink);   // busy-poll
//
// 用法（网络线程忙轮询）：同上。
#pragma once

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <hft/exchange/byte_sink.hpp>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <string>
#include <string_view>

namespace hft::net {

class UdpReceiver {
public:
  static constexpr std::size_t kBufSize = 2048;
  static constexpr int kMaxBatch = 32;

  // Bind to bind_addr:port. Use port 0 to let the OS pick a free port
  // (then query actual_port() to find out which one).
  // 绑定到 bind_addr:port。port=0 让 OS 自动分配（通过 actual_port() 查询）。
  UdpReceiver(std::string_view bind_addr, uint16_t port) noexcept {
    fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd_ < 0)
      return;

    int yes = 1;
    ::setsockopt(fd_, SOL_SOCKET, SO_REUSEPORT, &yes, sizeof(yes));
    ::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    // Enlarge kernel receive buffer to absorb bursts.
    // 扩大内核接收缓冲，吸收突发包。
    int rcvbuf = 4 * 1024 * 1024;
    ::setsockopt(fd_, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    const std::string addr_str(bind_addr);
    if (::inet_pton(AF_INET, addr_str.c_str(), &addr.sin_addr) <= 0) {
      ::close(fd_);
      fd_ = -1;
      return;
    }

    if (::bind(fd_, reinterpret_cast<const struct sockaddr *>(&addr),
               sizeof(addr)) < 0) {
      ::close(fd_);
      fd_ = -1;
      return;
    }

    init_slots();
  }

  ~UdpReceiver() noexcept {
    if (fd_ >= 0)
      ::close(fd_);
  }

  UdpReceiver(const UdpReceiver &) = delete;
  UdpReceiver &operator=(const UdpReceiver &) = delete;

  // Join a UDP multicast group.
  // iface_addr: local interface IP to receive on ("0.0.0.0" = any).
  // 加入 UDP 组播组。iface_addr：本地接收接口 IP（"0.0.0.0" = 任意）。
  bool join_multicast(std::string_view group_addr,
                      std::string_view iface_addr = "0.0.0.0") noexcept {
    if (fd_ < 0)
      return false;
    struct ip_mreq mreq{};
    const std::string g(group_addr), i(iface_addr);
    if (::inet_pton(AF_INET, g.c_str(), &mreq.imr_multiaddr) <= 0)
      return false;
    if (::inet_pton(AF_INET, i.c_str(), &mreq.imr_interface) <= 0)
      return false;
    return ::setsockopt(fd_, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq,
                        sizeof(mreq)) == 0;
  }

  // Receive up to max_msgs packets in one syscall; deliver each to sink.
  // Returns number of packets delivered. Returns 0 if nothing available.
  // Non-blocking: uses MSG_DONTWAIT — never blocks the calling thread.
  // 单次 syscall 最多收 max_msgs 个包，逐一交给 sink。
  // 非阻塞（MSG_DONTWAIT）：无包时立即返回 0，不阻塞调用线程。
  int recv_batch(exchange::IByteSink &sink, int max_msgs = kMaxBatch) noexcept {
    if (fd_ < 0)
      return 0;
    const int n = std::min(max_msgs, kMaxBatch);

    // recvmmsg may overwrite msg_namelen; restore before each call.
    // recvmmsg 可能修改 msg_namelen，每次调用前恢复。
    for (int i = 0; i < n; ++i)
      msgs_[i].msg_hdr.msg_namelen = sizeof(addrs_[i]);

    const int received = ::recvmmsg(fd_, msgs_.data(), static_cast<unsigned>(n),
                                    MSG_DONTWAIT, nullptr);
    if (received <= 0)
      return 0;

    for (int i = 0; i < received; ++i)
      sink.on_bytes(bufs_[i].data(), msgs_[i].msg_len);

    return received;
  }

  // Port the socket is actually bound to (useful when constructed with port 0).
  // 查询实际绑定端口（以 port=0 构造时有用）。
  uint16_t actual_port() const noexcept {
    if (fd_ < 0)
      return 0;
    struct sockaddr_in addr{};
    socklen_t len = sizeof(addr);
    if (::getsockname(fd_, reinterpret_cast<struct sockaddr *>(&addr), &len) <
        0)
      return 0;
    return ntohs(addr.sin_port);
  }

  bool ok() const noexcept { return fd_ >= 0; }
  int fd() const noexcept { return fd_; }

private:
  int fd_{-1};

  // Separate arrays so msgs_[] is contiguous — required by recvmmsg.
  // 分离数组使 msgs_[] 连续——recvmmsg 要求。
  std::array<std::array<std::byte, kBufSize>, kMaxBatch> bufs_{};
  std::array<struct iovec, kMaxBatch> iovecs_{};
  std::array<struct mmsghdr, kMaxBatch> msgs_{};
  std::array<struct sockaddr_in, kMaxBatch> addrs_{};

  void init_slots() noexcept {
    for (int i = 0; i < kMaxBatch; ++i) {
      iovecs_[i].iov_base = bufs_[i].data();
      iovecs_[i].iov_len = kBufSize;
      std::memset(&msgs_[i], 0, sizeof(msgs_[i]));
      msgs_[i].msg_hdr.msg_iov = &iovecs_[i];
      msgs_[i].msg_hdr.msg_iovlen = 1;
      msgs_[i].msg_hdr.msg_name = &addrs_[i];
      msgs_[i].msg_hdr.msg_namelen = sizeof(addrs_[i]);
    }
  }
};

} // namespace hft::net
