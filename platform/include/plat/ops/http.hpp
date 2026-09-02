#pragma once
// plat/ops/http.hpp — a deliberately tiny HTTP/1.1 server for the platops
// web console. Operator cadence, operator threat model: bind to loopback
// (default) and reach it over an SSH tunnel; one worker thread, one request
// per connection (Connection: close), no TLS, no keep-alive, no chunking.
// The trading processes never see this code — platops is a separate binary.

#include <atomic>
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <thread>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32")
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include "plat/core/log.hpp"

namespace plat::ops {

#ifdef _WIN32
using Sock = SOCKET;
inline constexpr Sock kBadSock = INVALID_SOCKET;
inline void close_sock(Sock s) { ::closesocket(s); }
#else
using Sock = int;
inline constexpr Sock kBadSock = -1;
inline void close_sock(Sock s) { ::close(s); }
#endif

struct HttpRequest {
  std::string method;
  std::string path;
  std::string body;
};

struct HttpResponse {
  int status = 200;
  std::string content_type = "text/plain; charset=utf-8";
  std::string body;
};

// Handler runs on the server thread; keep it quick (state snapshot reads,
// cmd-file writes) — one operator, human cadence.
using HttpHandler = std::function<HttpResponse(const HttpRequest&)>;

class HttpServer {
 public:
  ~HttpServer() { stop(); }

  bool start(const std::string& bind_addr, std::uint16_t port, HttpHandler handler) {
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;
#endif
    handler_ = std::move(handler);
    listen_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_ == kBadSock) return false;
    const int one = 1;
    ::setsockopt(listen_, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&one),
                 sizeof(one));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (::inet_pton(AF_INET, bind_addr.c_str(), &addr.sin_addr) != 1) {
      close_sock(listen_);
      listen_ = kBadSock;
      return false;
    }
    if (::bind(listen_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
        ::listen(listen_, 8) != 0) {
      close_sock(listen_);
      listen_ = kBadSock;
      return false;
    }
    running_.store(true);
    thr_ = std::thread([this] { loop(); });
    return true;
  }

  void stop() {
    if (!running_.exchange(false)) return;
    if (listen_ != kBadSock) close_sock(listen_);  // unblocks accept()
    listen_ = kBadSock;
    if (thr_.joinable()) thr_.join();
  }

 private:
  void loop() {
    while (running_.load()) {
      const Sock c = ::accept(listen_, nullptr, nullptr);
      if (c == kBadSock) {
        if (running_.load()) continue;  // transient accept error
        break;                          // listener closed by stop()
      }
      serve_one(c);
      close_sock(c);
    }
  }

  void serve_one(Sock c) {
    // Read headers (and however much body arrived with them).
    std::string in;
    char buf[4096];
    std::size_t hdr_end = std::string::npos;
    while (in.size() < 1 << 20) {
      const int n = ::recv(c, buf, sizeof(buf), 0);
      if (n <= 0) return;
      in.append(buf, static_cast<std::size_t>(n));
      hdr_end = in.find("\r\n\r\n");
      if (hdr_end != std::string::npos) break;
    }
    if (hdr_end == std::string::npos) return;

    HttpRequest req;
    {  // request line: METHOD SP PATH SP VERSION
      const std::size_t sp1 = in.find(' ');
      const std::size_t sp2 = sp1 == std::string::npos ? sp1 : in.find(' ', sp1 + 1);
      if (sp2 == std::string::npos) return;
      req.method = in.substr(0, sp1);
      req.path = in.substr(sp1 + 1, sp2 - sp1 - 1);
    }
    const std::size_t want = content_length(in.substr(0, hdr_end));
    std::string body = in.substr(hdr_end + 4);
    while (body.size() < want && body.size() < 1 << 20) {
      const int n = ::recv(c, buf, sizeof(buf), 0);
      if (n <= 0) break;
      body.append(buf, static_cast<std::size_t>(n));
    }
    req.body = std::move(body);

    const HttpResponse res = handler_ ? handler_(req) : HttpResponse{404, "text/plain", ""};
    std::string out = "HTTP/1.1 " + std::to_string(res.status) +
                      (res.status == 200 ? " OK" : " ERR") +
                      "\r\nContent-Type: " + res.content_type +
                      "\r\nContent-Length: " + std::to_string(res.body.size()) +
                      "\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n" + res.body;
    std::size_t sent = 0;
    while (sent < out.size()) {
      const int n = ::send(c, out.data() + sent, static_cast<int>(out.size() - sent), 0);
      if (n <= 0) break;
      sent += static_cast<std::size_t>(n);
    }
  }

  static std::size_t content_length(const std::string& hdrs) {
    // Case-insensitive scan for Content-Length.
    for (std::size_t i = 0; i + 15 < hdrs.size(); ++i) {
      if (_strnicmp_portable(hdrs.c_str() + i, "content-length:", 15) == 0) {
        return static_cast<std::size_t>(std::strtoull(hdrs.c_str() + i + 15, nullptr, 10));
      }
    }
    return 0;
  }
  static int _strnicmp_portable(const char* a, const char* b, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) {
      const int ca = a[i] >= 'A' && a[i] <= 'Z' ? a[i] + 32 : a[i];
      const int cb = b[i] >= 'A' && b[i] <= 'Z' ? b[i] + 32 : b[i];
      if (ca != cb) return ca - cb;
      if (ca == 0) return 0;
    }
    return 0;
  }

  std::atomic<bool> running_{false};
  std::thread thr_;
  Sock listen_ = kBadSock;
  HttpHandler handler_;
};

}  // namespace plat::ops
