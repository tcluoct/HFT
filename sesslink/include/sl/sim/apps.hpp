#pragma once
// The two applications that sit on top of the session layer in the bench and
// the tests — a toy venue and a toy firm. Toy *business* logic, but the
// recovery responsibilities they carry are the real ones:
//
//   VenueApp (server): acks every order, fills some — and drops DUPLICATE
//   tokens on the floor. That single line (the SeenRing probe) is what turns
//   the client's "resend everything unacked" policy from double-execution
//   into exactly-once execution.
//
//   FirmApp (client): keeps every order in an open-order map until its ack
//   is seen in the SEQUENCED stream, and on every (re)login resends whatever
//   is still open. Acks lost with the connection come back via journal
//   replay; orders lost with the connection go out again; orders that made
//   it but whose acks were in flight get resent AND deduped. All three
//   in-flight fates end in the same place — one execution, one ack.

#include <cstddef>
#include <cstring>

#include "sl/core/flat_hash.hpp"
#include "sl/core/session.hpp"
#include "sl/core/wire.hpp"

namespace sl {

struct VenueApp {
  SeenRing<1u << 16> seen;  // dedup window: 64k tokens ≫ any open window
  u32 fill_pm = 300;        // this many per mille of orders get one full fill
  u64 accepted = 0, dup_dropped = 0, fills = 0, rejected_len = 0;
  u64 exec_seq = 0;
  u64 rng = 0x243F6A8885A308D3ull;

  void on_unseq(const u8* p, std::size_t n, Journal& jr) {
    if (n != sizeof(OrderMsg)) {
      ++rejected_len;
      return;
    }
    OrderMsg o;
    std::memcpy(&o, p, sizeof o);
    if (!seen.insert(o.token)) {  // a resend of something already executed
      ++dup_dropped;
      return;
    }
    ++accepted;
    ExecMsg ack{};
    ack.token = o.token;
    ack.kind = kExecAck;
    ack.exec_id = ++exec_seq;
    ack.px = o.px;
    jr.append(&ack, sizeof ack);

    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    if (rng % 1000 < fill_pm) {
      ExecMsg fill{};
      fill.token = o.token;
      fill.fill_qty = o.qty;
      fill.kind = kExecFill;
      fill.exec_id = ++exec_seq;
      fill.px = o.px;
      jr.append(&fill, sizeof fill);
      ++fills;
    }
  }
};

struct FirmApp {
  FlatHash<OrderMsg, 1u << 13> open;  // token -> order still awaiting its ack
  u64 sent = 0, acked = 0, fills_seen = 0, resent = 0;
  u64 logins = 0, link_downs = 0, login_rejects = 0;
  u64 last_reject_reason = 0;
  u64 tail_at_login = 0;  // journal end reported by LoginAccept (replay target)
  u64 stream_digest = kFnvBasis;  // must equal the server journal's digest
  u64 seq_seen = 0;
  u64 last_acked_token = 0;

  // Send through the session and remember the order until it is acked.
  template <typename Session>
  bool send_order(Session& s, const OrderMsg& o) {
    if (!s.send_unseq(&o, sizeof o)) return false;
    open.insert(o.token, o);
    ++sent;
    return true;
  }

  // ---- ClientSession app interface ----
  template <typename Session>
  void on_login_accept(Session& s, u64 tail) {
    ++logins;
    tail_at_login = tail;
    // The in-flight ambiguity policy, in one line: everything unacked goes
    // out again. Replay may ack some of these before the resends land; the
    // venue's dedup ring absorbs the overlap.
    open.for_each([&](u64, OrderMsg& o) {
      if (s.send_unseq(&o, sizeof o)) ++resent;
    });
  }

  template <typename Session>
  void on_login_reject(Session&, u8 reason) {
    ++login_rejects;
    last_reject_reason = reason;
  }

  template <typename Session>
  void on_link_down(Session&) {
    ++link_downs;
  }

  template <typename Session>
  void on_sequenced(Session&, u64 seq, const u8* p, std::size_t n) {
    seq_seen = seq;
    stream_digest = fnv1a(p, n, stream_digest);
    if (n != sizeof(ExecMsg)) return;
    ExecMsg e;
    std::memcpy(&e, p, sizeof e);
    if (e.kind == kExecAck) {
      if (open.erase(e.token)) ++acked;
      last_acked_token = e.token;
    } else if (e.kind == kExecFill) {
      ++fills_seen;
    }
  }

  // Caught up with everything the server had at login time, and every order
  // we ever sent has its ack: the definition of "recovered".
  [[nodiscard]] bool recovered(u64 client_next_seq) const noexcept {
    return client_next_seq >= tail_at_login && open.empty();
  }
};

}  // namespace sl
