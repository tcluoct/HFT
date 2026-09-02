#pragma once
// The wire vocabulary of a SoupBinTCP-style session: a 3-byte frame header
// (2-byte big-endian length + 1-byte type) delimiting fixed-size binary
// payloads, and an incremental FrameReader that turns whatever TCP hands us —
// half a header, three frames glued together, one byte at a time — back into
// whole frames. TCP is a byte stream; message boundaries are the receiver's
// problem, and this file is where that problem is solved once.
//
// Payload integers are native-endian (both ends of a session are commodity
// x86 in this lab; wirecodec is the project that deals in byte order). The
// frame *length* is big-endian because that is the SoupBinTCP tradition and
// it keeps the header unambiguous in a packet dump.

#include <array>
#include <cstdint>
#include <cstring>

namespace sl {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using i64 = std::int64_t;

// ---- frame types (1 byte on the wire) --------------------------------------
// Server->client: A J S H Z    Client->server: L U R
inline constexpr u8 kLoginReq = 'L';      // client: start/resume a session
inline constexpr u8 kLoginAccept = 'A';   // server: replay begins at your seq
inline constexpr u8 kLoginReject = 'J';   // server: refused, see reason
inline constexpr u8 kSeqData = 'S';       // server: one sequenced payload
inline constexpr u8 kUnseqData = 'U';     // client: one unsequenced payload
inline constexpr u8 kSrvHeartbeat = 'H';  // server: alive, nothing to say
inline constexpr u8 kCliHeartbeat = 'R';  // client: alive, nothing to say
inline constexpr u8 kEndOfSession = 'Z';  // either: orderly goodbye

// Login reject reasons.
inline constexpr u8 kRejBadSession = 1;
inline constexpr u8 kRejSeqAhead = 2;  // client claims seqs the server never sent

#pragma pack(push, 1)
struct LoginReq {
  u64 session;   // which session (a day's stream) the client wants
  u64 next_seq;  // first sequenced message the client has NOT seen
};
struct LoginAccept {
  u64 session;
  u64 next_seq;  // server will (re)send starting here == req.next_seq
  u64 tail;      // journal end at accept time: replay is [next_seq, tail)
};
struct LoginReject {
  u8 reason;
};

// ---- the business payloads riding inside U (orders) and S (execs) ----------
// Deliberately tiny: sesslink is about the session, not the order semantics
// (omsd owns the order lifecycle; wirecodec owns schema evolution).
struct OrderMsg {
  u64 token;  // client-assigned, day-unique; the idempotency key
  u32 symbol;
  u32 qty;
  i64 px;
  u8 side;  // 'B' / 'S'
  u8 pad[7];
};
struct ExecMsg {
  u64 token;    // echoes the order's token
  u32 fill_qty; // 0 for an ack
  u8 kind;      // kExecAck / kExecFill / kExecRejected
  u8 pad[3];
  u64 exec_id;  // venue-assigned, strictly increasing
  i64 px;
};
#pragma pack(pop)

inline constexpr u8 kExecAck = 1;
inline constexpr u8 kExecFill = 2;
inline constexpr u8 kExecRejected = 3;

static_assert(sizeof(LoginReq) == 16 && sizeof(LoginAccept) == 24);
static_assert(sizeof(OrderMsg) == 32 && sizeof(ExecMsg) == 32);

// FNV-1a: the family's cheap stream digest (matchd proves replay with the
// same construction). Used here to prove client view == server journal.
inline constexpr u64 kFnvBasis = 0xcbf29ce484222325ull;
inline u64 fnv1a(const void* data, std::size_t n, u64 h = kFnvBasis) noexcept {
  const u8* p = static_cast<const u8*>(data);
  for (std::size_t i = 0; i < n; ++i) {
    h ^= p[i];
    h *= 0x100000001b3ull;
  }
  return h;
}

// ---- incremental frame parser ----------------------------------------------
// Feed it whatever recv() returned; it invokes f(type, payload, len) once per
// complete frame and keeps the ragged tail for next time. Returns false on a
// malformed length — the only unrecoverable framing error a length-prefixed
// stream can have — after which the connection must be dropped (there is no
// way to resynchronize a byte stream whose framing you no longer trust).
inline constexpr std::size_t kMaxFrame = 1024;  // type + payload, sanity bound

class FrameReader {
 public:
  template <typename F>
  [[nodiscard]] bool on_bytes(const u8* p, std::size_t n, F&& f) noexcept {
    while (n != 0) {
      // Top up the stitch buffer. kMaxFrame + 2 always fits in the buffer,
      // so a parse pass below always frees room for the next memcpy.
      const std::size_t take = n < buf_.size() - fill_ ? n : buf_.size() - fill_;
      std::memcpy(buf_.data() + fill_, p, take);
      fill_ += take;
      p += take;
      n -= take;

      const u8* cur = buf_.data();
      std::size_t avail = fill_;
      while (avail >= kHdr) {
        const u16 len = static_cast<u16>((static_cast<u16>(cur[0]) << 8) | cur[1]);
        if (len == 0 || len > kMaxFrame) return false;  // poisoned stream
        if (avail < kHdr + len) break;                  // frame still arriving
        f(cur[2], cur + 3, static_cast<std::size_t>(len) - 1);
        cur += kHdr + len;
        avail -= kHdr + len;
      }
      if (avail != 0 && cur != buf_.data()) std::memmove(buf_.data(), cur, avail);
      fill_ = avail;
    }
    return true;
  }

  // A new connection is a new byte stream: any half-frame from the old one
  // is garbage and must not be stitched onto fresh bytes.
  void reset() noexcept { fill_ = 0; }

 private:
  static constexpr std::size_t kHdr = 2;  // length prefix (type is inside len)
  std::array<u8, 4096> buf_{};
  std::size_t fill_ = 0;
};

// ---- frame writer helper -----------------------------------------------------
// Serializes header+payload into `dst` (must have 3+len room); returns bytes.
inline std::size_t write_frame(u8* dst, u8 type, const void* payload, std::size_t len) noexcept {
  const u16 wire_len = static_cast<u16>(len + 1);  // type + payload
  dst[0] = static_cast<u8>(wire_len >> 8);
  dst[1] = static_cast<u8>(wire_len & 0xff);
  dst[2] = type;
  if (len != 0) std::memcpy(dst + 3, payload, len);
  return 3 + len;
}

}  // namespace sl
