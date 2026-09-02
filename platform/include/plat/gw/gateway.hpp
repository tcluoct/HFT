#pragma once
// plat/gw/gateway.hpp — the counter gateway boundary (§9.4), as a concept:
// the site engine is templated on the concrete gateway type, so the
// risk-passed order handoff is a direct (inlinable) call — compile-time
// polymorphism, the same discipline sesslink applies to its ByteTransport.
// Everything vendor-specific lives behind this boundary; outcomes come back
// as Source ExecReport / SessionStateEvt envelopes through the driver's
// inbound ring, never as return values.
//
// Production implementation: plat/ctp/trader_client.hpp (SFIT CTP counter).
// The contract an implementation must honor:
//
//   · place/cancel are called on the engine thread only, after riskgate;
//   · every request produces exactly one terminal report — un-forwardable
//     requests are *bounced* (Reject / CancelReject), never swallowed;
//   · reports for one session arrive FIFO (a fill can never arrive after
//     its IOC's cancel-remainder);
//   · after a disconnect, order state is re-queried from the counter before
//     the session reports Up again — no blind resend, ever;
//   · state() == Up means authenticated, logged in, settlement-confirmed
//     (the pre-flight §11.3 reads it).

#include <concepts>

#include "plat/codec/msgs.hpp"
#include "plat/core/types.hpp"

namespace plat {

template <typename G>
concept OrderGateway = requires(G g, const G cg, const NewOrderReq& r, const CancelReq& c) {
  g.place(r);
  g.cancel(c);
  { cg.state() } -> std::same_as<SessState>;
};

}  // namespace plat
