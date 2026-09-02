#pragma once
// Non-blocking TCP, the first real network I/O in the hft family. Thin on
// purpose: connect/accept/send/recv plus exactly the three socket options a
// trading session cares about —
//   TCP_NODELAY  : disable Nagle. Nagle batches small writes waiting for an
//                  ACK; against delayed-ACK peers a ping-pong protocol
//                  serializes into tens of milliseconds per turn. Every
//                  order-entry session in production sets this.
//   SO_LINGER(0) : abort() sends RST instead of FIN — the honest way to
//                  simulate a crash for recovery drills (a real crash never
//                  says goodbye).
//   FIONBIO      : non-blocking everything; sessions busy-poll and never
//                  donate their core to the scheduler.
// Windows-first (Winsock), POSIX fallback so the family stays portable.

#include <cstddef>
#include <cstdint>

#if defined(_WIN32)
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #include <winsock2.h>
  #include <ws2tcpip.h>
#else
  #include <arpa/inet.h>
  #include <fcntl.h>
  #include <netinet/in.h>
  #include <netinet/tcp.h>
  #include <sys/socket.h>
  #include <unistd.h>
  #include <cerrno>
#endif

#include "sl/core/transport.hpp"

namespace sl {

#if defined(_WIN32)
using socket_t = SOCKET;
inline constexpr socket_t kBadSocket = INVALID_SOCKET;
#else
using socket_t = int;
inline constexpr socket_t kBadSocket = -1;
#endif

// One-time stack init (WSAStartup on Windows, no-op elsewhere).
inline bool net_init() noexcept {
#if defined(_WIN32)
  static const bool ok = [] {
    WSADATA wd;
    return ::WSAStartup(MAKEWORD(2, 2), &wd) == 0;
  }();
  return ok;
#else
  return true;
#endif
}

namespace detail {
inline void set_nonblocking(socket_t s) noexcept {
#if defined(_WIN32)
  u_long one = 1;
  ::ioctlsocket(s, FIONBIO, &one);
#else
  ::fcntl(s, F_SETFL, ::fcntl(s, F_GETFL, 0) | O_NONBLOCK);
#endif
}
inline void set_nodelay(socket_t s) noexcept {
  int one = 1;
  ::setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof one);
}
inline void close_sock(socket_t s) noexcept {
#if defined(_WIN32)
  ::closesocket(s);
#else
  ::close(s);
#endif
}
inline bool would_block() noexcept {
#if defined(_WIN32)
  return ::WSAGetLastError() == WSAEWOULDBLOCK;
#else
  return errno == EWOULDBLOCK || errno == EAGAIN;
#endif
}
}  // namespace detail

class TcpConn {
 public:
  TcpConn() = default;
  TcpConn(const TcpConn&) = delete;
  TcpConn& operator=(const TcpConn&) = delete;
  ~TcpConn() { close(); }

  // Blocking connect (setup path, not measured), then flip to non-blocking.
  bool connect_to(const char* ip, std::uint16_t port, bool nodelay = true) noexcept {
    if (!net_init()) return false;
    close();
    s_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s_ == kBadSocket) return false;
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    ::inet_pton(AF_INET, ip, &a.sin_addr);
    if (::connect(s_, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) {
      close();
      return false;
    }
    if (nodelay) detail::set_nodelay(s_);
    detail::set_nonblocking(s_);
    return true;
  }

  void adopt(socket_t s, bool nodelay) noexcept {
    close();
    s_ = s;
    if (nodelay) detail::set_nodelay(s_);
    detail::set_nonblocking(s_);
  }

  void close() noexcept {
    if (s_ != kBadSocket) {
      detail::close_sock(s_);
      s_ = kBadSocket;
    }
  }

  // Crash simulation: RST out, no FIN handshake, in-flight data dropped.
  void abort() noexcept {
    if (s_ == kBadSocket) return;
    linger lg{1, 0};
    ::setsockopt(s_, SOL_SOCKET, SO_LINGER, reinterpret_cast<const char*>(&lg), sizeof lg);
    close();
  }

  std::ptrdiff_t recv_some(std::uint8_t* p, std::size_t n) noexcept {
    const int r = ::recv(s_, reinterpret_cast<char*>(p), static_cast<int>(n), 0);
    if (r > 0) return r;
    if (r == 0) return kClosed;  // orderly FIN
    return detail::would_block() ? kWouldBlock : kClosed;
  }

  std::ptrdiff_t send_some(const std::uint8_t* p, std::size_t n) noexcept {
    const int r = ::send(s_, reinterpret_cast<const char*>(p), static_cast<int>(n), 0);
    if (r >= 0) return r;
    return detail::would_block() ? kWouldBlock : kClosed;
  }

  [[nodiscard]] bool valid() const noexcept { return s_ != kBadSocket; }

 private:
  socket_t s_ = kBadSocket;
};

static_assert(ByteTransport<TcpConn>);

class TcpListener {
 public:
  ~TcpListener() {
    if (s_ != kBadSocket) detail::close_sock(s_);
  }

  bool listen_on(std::uint16_t port) noexcept {
    if (!net_init()) return false;
    s_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s_ == kBadSocket) return false;
    int one = 1;
    ::setsockopt(s_, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&one), sizeof one);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::bind(s_, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) return false;
    if (::listen(s_, 8) != 0) return false;
    detail::set_nonblocking(s_);
    return true;
  }

  // Non-blocking accept; false = nobody knocking right now.
  bool accept_into(TcpConn& c, bool nodelay = true) noexcept {
    const socket_t s = ::accept(s_, nullptr, nullptr);
    if (s == kBadSocket) return false;
    c.adopt(s, nodelay);
    return true;
  }

 private:
  socket_t s_ = kBadSocket;
};

}  // namespace sl
