#pragma once
// A deterministic in-memory duplex "TCP connection" for the tests. Same
// ByteTransport shape as the real socket, but every awkward property of a
// byte stream is a knob instead of an accident:
//   * chunk_cap throttles how many bytes recv_some returns per call, so a
//     test can deliver a 35-byte frame one byte at a time, or glue a
//     thousand frames into one read — the framer must not care;
//   * kill() models a crash: both directions close and bytes in flight
//     VANISH (exactly what a dead TCP peer means for unacked data) — the
//     scenario that makes resend-with-dedup necessary at all;
//   * reset() is "the client dialed back in": fresh pipes, same endpoints.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include "sl/core/transport.hpp"

namespace sl {

struct MemPipe {
  std::vector<std::uint8_t> q;
  std::size_t head = 0;
  bool closed = false;

  void clear_all() {
    q.clear();
    head = 0;
  }
};

class MemEnd {
 public:
  MemEnd(MemPipe& in, MemPipe& out) : in_(&in), out_(&out) {}

  std::ptrdiff_t recv_some(std::uint8_t* p, std::size_t n) noexcept {
    const std::size_t avail = in_->q.size() - in_->head;
    if (avail == 0) return in_->closed ? kClosed : kWouldBlock;
    if (recv_budget == 0) return kWouldBlock;  // scripted starvation
    std::size_t take = avail < n ? avail : n;
    if (take > chunk_cap) take = chunk_cap;
    if (take > recv_budget) take = recv_budget;
    recv_budget -= take;
    std::memcpy(p, in_->q.data() + in_->head, take);
    in_->head += take;
    if (in_->head == in_->q.size()) in_->clear_all();
    return static_cast<std::ptrdiff_t>(take);
  }

  std::ptrdiff_t send_some(const std::uint8_t* p, std::size_t n) noexcept {
    if (out_->closed) return kClosed;
    std::size_t take = n;
    if (take > chunk_cap) take = chunk_cap;  // partial writes too
    out_->q.insert(out_->q.end(), p, p + take);
    return static_cast<std::ptrdiff_t>(take);
  }

  std::size_t chunk_cap = ~std::size_t{0};    // max bytes moved per call
  std::size_t recv_budget = ~std::size_t{0};  // total bytes this end may still read

 private:
  MemPipe* in_;
  MemPipe* out_;
};

static_assert(ByteTransport<MemEnd>);

struct MemLink {
  MemPipe c2s, s2c;
  MemEnd client{s2c, c2s};  // client reads what server wrote
  MemEnd server{c2s, s2c};

  void kill() {  // crash: undelivered bytes are gone forever
    c2s.closed = s2c.closed = true;
    c2s.clear_all();
    s2c.clear_all();
  }
  void reset() {  // reconnect: a brand-new byte stream between old endpoints
    c2s.closed = s2c.closed = false;
    c2s.clear_all();
    s2c.clear_all();
  }
};

}  // namespace sl
