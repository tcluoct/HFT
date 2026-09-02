#pragma once
// The vocabulary of an order-management system: what the strategy asks for
// (Request), what the venue reports back (ExecReport), the seven lifecycle
// states an order can be in, and — the heart of the project — the exhaustive
// state x event verdict matrix.
//
// An OMS's difficulty is not throughput; it is that the strategy's requests
// and the venue's reports are two async streams that CROSS. A fill can race
// your cancel. A replace-ack can arrive after the fill that made it moot.
// A lost ack can leave you pending-new for an order the venue is happily
// filling. Every one of those is a (state, event) pair, and the matrix
// classifies all 56 of them explicitly:
//
//   kApply    — the expected transition
//   kRace     — legal only because a request of ours is still in flight;
//               correct handling is state-dependent, and each is a test case
//   kHeal     — explainable only by a lost/dropped earlier report; apply the
//               evidence, count the suspicion (drop-copy recon confirms)
//   kStale    — a late answer to a question that no longer matters; ignore
//   kVenueErr — the venue broke its ordering contract (fill after cancel-ack,
//               overfill, ...); never apply, always flag
//
// Nothing is implicit: a pair the designer never thought about cannot slip
// through, because every pair had to be written down.

#include <cstdint>

namespace om {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using i64 = std::int64_t;

// ---- streams ---------------------------------------------------------------

enum class ReqType : u8 { kNew = 0, kCancel = 1, kReplace = 2 };

struct Request {
  u64 cl_id;   // client order id (token): day-unique, strategy-assigned
  i64 px;
  u32 qty;     // kNew: order qty; kReplace: NEW total qty
  u32 sym;
  ReqType type;
  u8 side;     // 0 = buy, 1 = sell
  u8 pad[6];
};
static_assert(sizeof(Request) == 32);

enum class Ev : u8 {
  kAckNew = 0,
  kRejNew = 1,
  kFill = 2,
  kAckCancel = 3,
  kRejCancel = 4,
  kAckReplace = 5,
  kRejReplace = 6,
  kUnsolCancel = 7,  // venue-initiated (STP, session cancel, halt...)
};
inline constexpr unsigned kEvCount = 8;

struct ExecReport {
  u64 cl_id;
  u64 exec_id;  // venue-assigned, unique; the dedup key
  i64 px;
  u32 qty;      // kFill: fill qty; kAckReplace: new TOTAL qty; else 0
  Ev ev;
  u8 pad[3];
};
static_assert(sizeof(ExecReport) == 32);

// ---- order lifecycle ---------------------------------------------------------

enum class St : u8 {
  kPendingNew = 0,      // sent, no venue answer yet
  kLive = 1,            // acked, has leaves
  kPendingCancel = 2,   // our cancel is in flight
  kPendingReplace = 3,  // our replace is in flight
  kFilled = 4,          // terminal
  kCanceled = 5,        // terminal
  kRejected = 6,        // terminal
};
inline constexpr unsigned kStCount = 7;

inline constexpr bool is_terminal(St s) noexcept {
  return s == St::kFilled || s == St::kCanceled || s == St::kRejected;
}

// The four categories an order's fate collapses to once the crossings are
// resolved — the level at which OMS and venue must agree at end of day.
enum class Cat : u8 { kOpen = 0, kFilled = 1, kCanceled = 2, kRejected = 3 };

enum class Verdict : u8 { kApply = 0, kRace = 1, kHeal = 2, kStale = 3, kVenueErr = 4 };
inline constexpr unsigned kVerdictCount = 5;

// The matrix. Rows = current state, columns = venue event.
//   cols: AckNew    RejNew    Fill      AckCxl    RejCxl    AckRpl    RejRpl    UnsolCxl
inline constexpr Verdict kVerdict[kStCount][kEvCount] = {
    // kPendingNew: nothing acked yet — Fill/AckCancel/AckReplace here mean the
    // ack-new was lost (heal); RejCancel/RejReplace mean OUR later request
    // overtook nothing (venue answered it first): pure crossing (race).
    {Verdict::kApply, Verdict::kApply, Verdict::kHeal, Verdict::kHeal, Verdict::kRace,
     Verdict::kHeal, Verdict::kRace, Verdict::kHeal},
    // kLive: Fill/UnsolCancel are the normal life of a resting order. A second
    // AckNew is a venue bug. AckCancel/AckReplace without a pending request of
    // ours imply a lost transition (heal). RejCancel/RejReplace are answers to
    // questions we no longer have open (stale).
    {Verdict::kVenueErr, Verdict::kVenueErr, Verdict::kApply, Verdict::kHeal, Verdict::kStale,
     Verdict::kHeal, Verdict::kStale, Verdict::kApply},
    // kPendingCancel: everything can legally cross an in-flight cancel — the
    // fill-vs-cancel race lives here. AckCancel/RejCancel are the answers.
    {Verdict::kRace, Verdict::kRace, Verdict::kRace, Verdict::kApply, Verdict::kApply,
     Verdict::kRace, Verdict::kRace, Verdict::kRace},
    // kPendingReplace: fills keep landing on the old order while the replace
    // is in flight; AckReplace/RejReplace resolve it.
    {Verdict::kRace, Verdict::kRace, Verdict::kRace, Verdict::kHeal, Verdict::kStale,
     Verdict::kApply, Verdict::kApply, Verdict::kRace},
    // kFilled (terminal): a further fill is an OVERFILL; cancel/replace acks
    // for a filled order violate venue ordering; their rejections are the
    // expected "too late" answers.
    {Verdict::kVenueErr, Verdict::kVenueErr, Verdict::kVenueErr, Verdict::kVenueErr,
     Verdict::kStale, Verdict::kVenueErr, Verdict::kStale, Verdict::kVenueErr},
    // kCanceled (terminal): a fill after cancel-ack is the classic venue
    // contract violation. Late cancel echoes are noise.
    {Verdict::kVenueErr, Verdict::kVenueErr, Verdict::kVenueErr, Verdict::kStale,
     Verdict::kStale, Verdict::kVenueErr, Verdict::kStale, Verdict::kStale},
    // kRejected (terminal): the order never existed at the venue; only late
    // rejections of our follow-ups are benign.
    {Verdict::kVenueErr, Verdict::kStale, Verdict::kVenueErr, Verdict::kVenueErr,
     Verdict::kStale, Verdict::kVenueErr, Verdict::kStale, Verdict::kVenueErr},
};

// Spot-check the pairs people argue about in design reviews.
static_assert(kVerdict[unsigned(St::kPendingCancel)][unsigned(Ev::kFill)] == Verdict::kRace,
              "a fill may race an in-flight cancel");
static_assert(kVerdict[unsigned(St::kCanceled)][unsigned(Ev::kFill)] == Verdict::kVenueErr,
              "a fill after cancel-ack is a venue contract violation");
static_assert(kVerdict[unsigned(St::kFilled)][unsigned(Ev::kFill)] == Verdict::kVenueErr,
              "an overfill is never applied");
static_assert(kVerdict[unsigned(St::kPendingNew)][unsigned(Ev::kFill)] == Verdict::kHeal,
              "a fill before ack-new implies the ack was lost");

// ---- the order record ---------------------------------------------------------

struct Order {
  u64 cl_id = 0;
  u32 qty = 0;          // last known total qty (updated by AckReplace)
  u32 cum = 0;          // filled so far
  u32 pending_qty = 0;  // requested total qty while kPendingReplace
  u32 reserved = 0;     // leaves currently counted into open exposure
  i64 px = 0;
  u32 sym = 0;
  u8 side = 0;
  St st = St::kPendingNew;
  u8 pad[2] = {};
};
static_assert(sizeof(Order) == 40);

// Per-symbol exposure book: position from fills, open interest from reserves.
struct SymBook {
  i64 position = 0;
  u64 open_buy = 0;
  u64 open_sell = 0;
};

// FNV-style running digest of every applied transition: two runs of the same
// input tape must produce the same value — matchd's determinism proof, for
// order state.
inline void digest_mix(u64& h, u64 v) noexcept {
  h ^= v;
  h *= 0x100000001b3ull;
}

}  // namespace om
