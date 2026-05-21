// include/hft/oe/tcp_session.hpp
// Blocking TCP session: connect, send, recv with automatic reconnect.
// 阻塞式 TCP 会话：连接、发送、接收，支持自动重连。
//
// Design note: order sending is latency-critical but low-volume (< 10k/s).
// A blocking send() on a kernel-bypass NIC is effectively zero-copy and
// returns in < 1 µs. For standard kernel TCP, send() is still fast enough
// if the socket send buffer is warm. Async I/O adds complexity for no gain
// here.
//
// 设计说明：发单延迟敏感但量小（< 1万/秒）。内核旁路 NIC 的阻塞 send() 实际上
// 是零拷贝，耗时 < 1µs。标准内核 TCP 的 send() 在 socket 发送缓冲区热的情况下
// 也足够快。异步 I/O 在此处只增加复杂度，没有收益。
#pragma once

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <string>
#include <string_view>

namespace hft::oe {

class TcpSession {
public:
  TcpSession(std::string_view host, uint16_t port) noexcept
      : host_(host), port_(port) {}

  ~TcpSession() noexcept { close(); }

  TcpSession(const TcpSession &) = delete;
  TcpSession &operator=(const TcpSession &) = delete;

  // Connect (or reconnect). Returns true on success.
  // 连接（或重连）。成功返回 true。
  bool connect() noexcept {
    close();

    fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd_ < 0)
      return false;

    // Disable Nagle — we send small messages and need minimal latency.
    // 禁用 Nagle 算法——发送小消息，需要最低延迟。
    int yes = 1;
    ::setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port_);
    if (::inet_pton(AF_INET, host_.c_str(), &addr.sin_addr) <= 0) {
      close();
      return false;
    }

    if (::connect(fd_, reinterpret_cast<const struct sockaddr *>(&addr),
                  sizeof(addr)) < 0) {
      close();
      return false;
    }

    connected_ = true;
    return true;
  }

  // Send all bytes synchronously. Returns false on error (sets
  // connected_=false). 同步发送所有字节。出错时返回 false 并置
  // connected_=false。
  bool send_all(const std::byte *data, std::size_t len) noexcept {
    if (!connected_)
      return false;
    std::size_t sent = 0;
    while (sent < len) {
      const ssize_t n = ::send(fd_, reinterpret_cast<const char *>(data) + sent,
                               len - sent, MSG_NOSIGNAL);
      if (n <= 0) {
        connected_ = false;
        return false;
      }
      sent += static_cast<std::size_t>(n);
    }
    return true;
  }

  // Non-blocking recv: returns bytes read, 0 if nothing available, -1 on error.
  // 非阻塞接收：返回读取字节数，0 表示暂无数据，-1 表示出错。
  ssize_t recv(std::byte *buf, std::size_t cap) noexcept {
    if (!connected_)
      return -1;
    const ssize_t n =
        ::recv(fd_, reinterpret_cast<char *>(buf), cap, MSG_DONTWAIT);
    if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
      connected_ = false;
      return -1;
    }
    return (n < 0) ? 0 : n;
  }

  void close() noexcept {
    if (fd_ >= 0) {
      ::close(fd_);
      fd_ = -1;
    }
    connected_ = false;
  }

  bool connected() const noexcept { return connected_; }
  int fd() const noexcept { return fd_; }

private:
  std::string host_;
  uint16_t port_;
  int fd_{-1};
  bool connected_{false};
};

} // namespace hft::oe
