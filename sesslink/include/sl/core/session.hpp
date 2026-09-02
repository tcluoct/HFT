#pragma once
// The session layer itself: the piece of an HFT stack that turns a raw TCP
// connection into a *resumable, gapless, exactly-once* order channel.
//
// The design mirrors SoupBinTCP (the NASDAQ session protocol that carries
// OUCH), reduced to its load-bearing ideas:
//
//   * The server journals every sequenced payload it ever emits. The journal
//     IS the send queue: each connection holds a cursor into it, and
//     "replay after reconnect" and "live streaming" are the same loop —
//     push journal[cursor..tail] into the socket as fast as it drains.
//     Recovery is not a special mode; it is being behind.
//   * The client counts sequenced messages from its login point (sequence
//     numbers are implicit — never on the wire per message), so a gap is
//     structurally impossible: whatever arrives IS the next message.
//   * Client requests (orders) are unsequenced and carry a client token; the
//     server's dedup ring makes delivery *effects* exactly-once even though
//     TCP after a crash gives you at-most-once with no receipt. The client
//     resends everything unacked after login; duplicates are absorbed, holes
//     are filled. This pair — replay for the down-stream, token dedup for
//     the up-stream — is the whole answer to in-flight ambiguity.
//   * Both sides run heartbeat timers (send when idle, declare the peer dead
//     when silent too long) because TCP alone can take many minutes to
//     notice a dead peer, and a trading session cannot.
//
// Sessions are single-threaded and non-blocking; poll(now) pumps rx, tx and
// timers. Time is passed in (nanoseconds), never read from a clock inside —
// that is what makes the tests able to script timeouts deterministically.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

#include "sl/core/flat_hash.hpp"
#include "sl/core/transport.hpp"
#include "sl/core/wire.hpp"

namespace sl {

// ---- journal ----------------------------------------------------------------
// Fixed 32-byte records, 1-based sequence numbers. append() never rewrites —
// the journal is the immutable history of the session's outbound stream, and
// digest() over it is the ground truth every recovery test compares against.
class Journal {
 public:
  static constexpr std::size_t kRecSize = 32;

  explicit Journal(std::size_t reserve_records = 1u << 20) {
    bytes_.reserve(reserve_records * kRecSize);
  }

  void append(const void* payload, std::size_t len) {
    u8 rec[kRecSize] = {};
    std::memcpy(rec, payload, len < kRecSize ? len : kRecSize);
    bytes_.insert(bytes_.end(), rec, rec + kRecSize);
    ++count_;
  }

  [[nodiscard]] u64 next_seq() const noexcept { return count_ + 1; }
  [[nodiscard]] u64 count() const noexcept { return count_; }
  [[nodiscard]] const u8* at(u64 seq) const noexcept {
    return bytes_.data() + (seq - 1) * kRecSize;
  }
  [[nodiscard]] u64 digest() const noexcept {
    return fnv1a(bytes_.data(), bytes_.size());
  }

 private:
  std::vector<u8> bytes_;
  u64 count_ = 0;
};

// ---- tx ring ------------------------------------------------------------------
// Outbound byte queue for a non-blocking socket: push() frames in, flush()
// moves what the kernel will take right now and keeps the rest. Fixed
// capacity — when it is full we simply stop advancing the journal cursor,
// which is backpressure by construction, not by exception.
class TxRing {
 public:
  static constexpr std::size_t kCap = 1u << 20;

  bool push(u8 type, const void* payload, std::size_t len) noexcept {
    const std::size_t need = 3 + len;
    if (kCap - size() < need) return false;
    u8 tmp[3 + kMaxFrame];
    write_frame(tmp, type, payload, len);
    for (std::size_t i = 0; i < need; ++i) buf_[(tail_ + i) & kMask] = tmp[i];
    tail_ += need;
    return true;
  }

  // Returns kClosed if the transport died, else bytes moved this call.
  template <ByteTransport T>
  std::ptrdiff_t flush(T& t) noexcept {
    std::ptrdiff_t moved = 0;
    while (head_ != tail_) {
      const std::size_t at = head_ & kMask;
      const std::size_t run = size() < kCap - at ? size() : kCap - at;
      const std::ptrdiff_t r = t.send_some(&buf_[at], run);
      if (r == kClosed) return kClosed;
      if (r == kWouldBlock || r == 0) break;
      head_ += static_cast<std::size_t>(r);
      moved += r;
      if (static_cast<std::size_t>(r) < run) break;  // kernel buffer full
    }
    return moved;
  }

  [[nodiscard]] std::size_t size() const noexcept { return tail_ - head_; }
  [[nodiscard]] bool empty() const noexcept { return head_ == tail_; }
  void clear() noexcept { head_ = tail_ = 0; }

 private:
  static constexpr std::size_t kMask = kCap - 1;
  std::unique_ptr<u8[]> buf_ = std::make_unique<u8[]>(kCap);
  std::size_t head_ = 0, tail_ = 0;  // monotonic; index mod kCap
};

struct HeartbeatCfg {
  u64 interval_ns = 1'000'000'000;  // send one after this much tx silence
  u64 timeout_ns = 5'000'000'000;   // declare peer dead after this much rx silence
};

// ---- server ---------------------------------------------------------------
// ServerCore outlives connections: journal + application + dedup survive a
// client crash by design — they ARE the state that makes resumption work.
// The App contributes business logic only: on_unseq(payload, len, journal&),
// appending whatever sequenced responses it wants published.
template <typename App>
struct ServerCore {
  Journal journal;
  App app;
  u64 session_id = 1;
  u64 logins = 0, rejects = 0;
};

enum class SrvState : u8 { kAwaitLogin, kEstablished, kDrainThenDie, kDead };

template <ByteTransport Transport, typename App>
class ServerSession {
 public:
  ServerSession(ServerCore<App>& core, Transport& t, u64 now, HeartbeatCfg hb = {})
      : core_(&core), t_(&t), hb_(hb), last_rx_(now), last_push_(now) {}

  void poll(u64 now) {
    if (state_ == SrvState::kDead) return;

    // rx pump: drain the socket, feed the framer.
    u8 buf[16384];
    for (;;) {
      const std::ptrdiff_t r = t_->recv_some(buf, sizeof buf);
      if (r == kWouldBlock) break;
      if (r == kClosed) {
        state_ = SrvState::kDead;
        return;
      }
      last_rx_ = now;
      const bool ok = rx_.on_bytes(buf, static_cast<std::size_t>(r),
                                   [&](u8 ty, const u8* p, std::size_t n) { on_frame(ty, p, n); });
      if (!ok) {
        ++proto_errors_;
        state_ = SrvState::kDead;
        return;
      }
      if (state_ == SrvState::kDead) return;
      if (static_cast<std::size_t>(r) < sizeof buf) break;
    }

    // tx pump: chase the journal tail. Replay and live are this same loop.
    if (state_ == SrvState::kEstablished) {
      while (cursor_ < core_->journal.next_seq() &&
             tx_.push(kSeqData, core_->journal.at(cursor_), Journal::kRecSize)) {
        ++cursor_;
        last_push_ = now;
      }
      if (now - last_push_ >= hb_.interval_ns && tx_.push(kSrvHeartbeat, nullptr, 0))
        last_push_ = now;
      if (now - last_rx_ >= hb_.timeout_ns) {  // client silent: presumed dead
        state_ = SrvState::kDead;
        return;
      }
    }

    if (tx_.flush(*t_) == kClosed) state_ = SrvState::kDead;
    if (state_ == SrvState::kDrainThenDie && tx_.empty()) state_ = SrvState::kDead;
  }

  [[nodiscard]] bool alive() const noexcept { return state_ != SrvState::kDead; }
  [[nodiscard]] SrvState state() const noexcept { return state_; }
  [[nodiscard]] u64 caught_up_seq() const noexcept { return cursor_; }
  [[nodiscard]] u64 proto_errors() const noexcept { return proto_errors_; }

 private:
  void on_frame(u8 type, const u8* p, std::size_t n) {
    if (state_ == SrvState::kDead || state_ == SrvState::kDrainThenDie) return;
    switch (type) {
      case kLoginReq: {
        if (n != sizeof(LoginReq) || state_ != SrvState::kAwaitLogin) {
          ++proto_errors_;
          state_ = SrvState::kDead;
          return;
        }
        LoginReq req;
        std::memcpy(&req, p, sizeof req);
        if (req.session != core_->session_id) {
          reject(kRejBadSession);
          return;
        }
        if (req.next_seq == 0 || req.next_seq > core_->journal.next_seq()) {
          reject(kRejSeqAhead);  // claims to have seen messages never sent
          return;
        }
        ++core_->logins;
        cursor_ = req.next_seq;
        const LoginAccept la{req.session, req.next_seq, core_->journal.next_seq()};
        tx_.push(kLoginAccept, &la, sizeof la);
        state_ = SrvState::kEstablished;
        break;
      }
      case kUnseqData:
        if (state_ != SrvState::kEstablished) {
          ++proto_errors_;
          state_ = SrvState::kDead;
          return;
        }
        core_->app.on_unseq(p, n, core_->journal);
        break;
      case kCliHeartbeat:
        break;  // last_rx_ already refreshed by the pump
      case kEndOfSession:
        state_ = SrvState::kDrainThenDie;
        break;
      default:
        ++proto_errors_;
        state_ = SrvState::kDead;
        break;
    }
  }

  void reject(u8 reason) {
    ++core_->rejects;
    const LoginReject rj{reason};
    tx_.push(kLoginReject, &rj, sizeof rj);
    state_ = SrvState::kDrainThenDie;
  }

  ServerCore<App>* core_;
  Transport* t_;
  HeartbeatCfg hb_;
  FrameReader rx_;
  TxRing tx_;
  SrvState state_ = SrvState::kAwaitLogin;
  u64 cursor_ = 1;  // next journal seq to hand to the socket
  u64 last_rx_, last_push_;
  u64 proto_errors_ = 0;
};

// ---- client -----------------------------------------------------------------
// The App sees a clean stream: on_sequenced(session, seq, payload, len) with
// seq guaranteed contiguous across any number of reconnects, plus login /
// link-down notifications (on_login_accept is where an app resends unacked
// orders — see sim/apps.hpp FirmApp). The ClientSession object persists
// across connections; attach() binds it to a fresh transport and resumes
// from next_seq_ — the one number that carries the whole session's progress.
enum class CliState : u8 { kIdle, kLoginSent, kEstablished, kDead };

template <ByteTransport Transport, typename App>
class ClientSession {
 public:
  explicit ClientSession(App& app, u64 session_id = 1, HeartbeatCfg hb = {})
      : app_(&app), session_id_(session_id), hb_(hb) {}

  // Bind to a freshly connected transport and ask to resume where we left
  // off. Everything connection-scoped (framer, tx queue) resets; everything
  // session-scoped (next_seq_) survives — that asymmetry IS resumption.
  void attach(Transport& t, u64 now) {
    t_ = &t;
    now_ = now;
    rx_.reset();
    tx_.clear();
    state_ = CliState::kLoginSent;
    last_rx_ = now;
    last_push_ = now;
    const LoginReq lr{session_id_, next_seq_};
    tx_.push(kLoginReq, &lr, sizeof lr);
    tx_.flush(*t_);
  }

  void poll(u64 now) {
    if (state_ == CliState::kDead || state_ == CliState::kIdle) return;
    now_ = now;

    u8 buf[16384];
    for (;;) {
      const std::ptrdiff_t r = t_->recv_some(buf, sizeof buf);
      if (r == kWouldBlock) break;
      if (r == kClosed) {
        down();
        return;
      }
      last_rx_ = now;
      const bool ok = rx_.on_bytes(buf, static_cast<std::size_t>(r),
                                   [&](u8 ty, const u8* p, std::size_t n) { on_frame(ty, p, n); });
      if (!ok) {
        ++proto_errors_;
        down();
        return;
      }
      if (state_ == CliState::kDead) return;
      if (static_cast<std::size_t>(r) < sizeof buf) break;
    }

    if (now - last_push_ >= hb_.interval_ns && tx_.push(kCliHeartbeat, nullptr, 0))
      last_push_ = now;
    if (now - last_rx_ >= hb_.timeout_ns) {  // server silent: link is dead
      down();
      return;
    }
    if (tx_.flush(*t_) == kClosed) down();
  }

  // Orders ride unsequenced. Only meaningful once established (before login
  // is accepted the server would kill the connection).
  bool send_unseq(const void* payload, std::size_t len) noexcept {
    if (state_ != CliState::kEstablished) return false;
    const bool ok = tx_.push(kUnseqData, payload, len);
    if (ok) last_push_ = now_;  // data is liveness; no heartbeat needed on a busy link
    return ok;
  }

  [[nodiscard]] CliState state() const noexcept { return state_; }
  [[nodiscard]] u64 next_seq() const noexcept { return next_seq_; }
  [[nodiscard]] u64 proto_errors() const noexcept { return proto_errors_; }

 private:
  void down() {
    state_ = CliState::kDead;
    app_->on_link_down(*this);
  }

  void on_frame(u8 type, const u8* p, std::size_t n) {
    if (state_ == CliState::kDead) return;
    switch (type) {
      case kLoginAccept: {
        if (n != sizeof(LoginAccept) || state_ != CliState::kLoginSent) {
          ++proto_errors_;
          down();
          return;
        }
        LoginAccept la;
        std::memcpy(&la, p, sizeof la);
        state_ = CliState::kEstablished;
        app_->on_login_accept(*this, la.tail);
        break;
      }
      case kLoginReject:
        app_->on_login_reject(*this, n >= 1 ? p[0] : 0);
        state_ = CliState::kDead;
        break;
      case kSeqData:
        app_->on_sequenced(*this, next_seq_, p, n);
        ++next_seq_;
        break;
      case kSrvHeartbeat:
        break;
      case kEndOfSession:
        down();
        break;
      default:
        ++proto_errors_;
        down();
        break;
    }
  }

  App* app_;
  Transport* t_ = nullptr;
  u64 session_id_;
  HeartbeatCfg hb_;
  FrameReader rx_;
  TxRing tx_;
  CliState state_ = CliState::kIdle;
  u64 next_seq_ = 1;  // next sequenced message we have NOT seen (survives reconnects)
  u64 now_ = 0;       // stamped at poll()/attach(); lets callbacks send without a clock
  u64 last_rx_ = 0, last_push_ = 0;
  u64 proto_errors_ = 0;
};

}  // namespace sl
