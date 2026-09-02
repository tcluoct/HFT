#pragma once
// Inbound order-entry protocol, modeled on NASDAQ OUCH: fixed-size packed
// binary messages, one char discriminator. All three inbound types share a
// 32-byte frame so the journal is a flat array of 32-byte records.

#include <cstdint>

#include "me/core/types.hpp"

namespace me {

inline constexpr char kMsgEnter = 'O';
inline constexpr char kMsgCancel = 'X';
inline constexpr char kMsgReplace = 'U';

#pragma pack(push, 1)

struct EnterOrder {
  char type;              // 'O'
  std::uint8_t side;      // kBuy / kSell
  std::uint8_t tif;       // kTifDay / kTifIoc / kTifFok
  std::uint8_t pad0;
  ParticipantId participant;
  SymbolId symbol;
  Token token;            // client's identifier, echoed on every report
  Price price;
  Qty qty;
  std::uint8_t pad1[4];
};
static_assert(sizeof(EnterOrder) == 32);

struct CancelOrder {
  char type;              // 'X'
  std::uint8_t pad0;
  ParticipantId participant;
  std::uint8_t pad1[4];
  OrderId order_id;       // exchange-assigned handle
  Token token;
  std::uint8_t pad2[8];
};
static_assert(sizeof(CancelOrder) == 32);

struct ReplaceOrder {
  char type;              // 'U'
  std::uint8_t pad0;
  ParticipantId participant;
  std::uint8_t pad1[4];
  OrderId order_id;
  Price new_price;
  Qty new_qty;
  std::uint8_t pad2[4];
};
static_assert(sizeof(ReplaceOrder) == 32);

union InMsg {
  char type;
  EnterOrder enter;
  CancelOrder cancel;
  ReplaceOrder replace;
  unsigned char raw[32];
};
static_assert(sizeof(InMsg) == 32);

#pragma pack(pop)

// Outbound events are delivered through a compile-time Sink (no virtuals,
// no serialization in the core). A Sink must provide:
//
//   void on_accepted(ParticipantId, Token, OrderId, uint8_t side, Price, Qty);
//   void on_canceled(ParticipantId, Token, OrderId, Qty leaves, CancelReason);
//   void on_replaced(ParticipantId, Token, OrderId old_id, OrderId new_id, Price, Qty);
//   void on_rejected(ParticipantId, Token, RejectReason);
//   void on_exec(ParticipantId, Token, OrderId, Qty, Price);      // one per side per trade
//   void on_md_add(OrderId, uint8_t side, Price, Qty);            // anonymous market data
//   void on_md_exec(OrderId, Qty, Price);
//   void on_md_reduce(OrderId, Qty dec);
//   void on_md_delete(OrderId);

}  // namespace me
