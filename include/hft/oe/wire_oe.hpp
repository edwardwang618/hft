// include/hft/oe/wire_oe.hpp
// Order Entry binary wire format v1.
// 订单报入二进制线路格式 v1。
//
// Little-endian, packed (no alignment padding).
// All messages share a fixed 16-byte header.
// 小端序，紧凑编码（无对齐填充）。所有消息共享 16 字节固定头部。
//
// ┌─────────────────────────────────────────────┐
// │ Header (16 B)                               │
// │   u8  msg_type                              │
// │   u8  _reserved  (must be 0)                │
// │   u16 msg_len    (entire message incl hdr)  │
// │   u64 clord_id   (client-assigned order id) │
// │   u32 sym                                   │
// └─────────────────────────────────────────────┘
//
// Client → Exchange:
//   NewOrder  (type=1): hdr + side:u8 + _pad:u8 + _pad2:u16 + px:i64 + qty:i64
//   Cancel    (type=2): hdr + exch_id:u64
//   Replace   (type=3): hdr + exch_id:u64 + px:i64 + qty:i64
//
// Exchange → Client (execution reports):
//   Ack       (type=0x10): hdr + exch_id:u64
//   Fill      (type=0x11): hdr + exch_id:u64 + filled_qty:i64 + px:i64
//   CancelAck (type=0x12): hdr + exch_id:u64
//   Reject    (type=0x13): hdr + reason: char[32] (null-padded)
//
#pragma once

#include <cstddef>
#include <cstdint>

namespace hft::oe::wire {

// ── Message type codes ────────────────────────────────────────────────
enum MsgType : uint8_t {
  // Client → Exchange
  kNewOrder = 0x01,
  kCancel = 0x02,
  kReplace = 0x03,
  // Exchange → Client
  kAck = 0x10,
  kFill = 0x11,
  kCancelAck = 0x12,
  kReject = 0x13,
};

static constexpr size_t kRejectReasonLen = 32;

// ── Packed structs ────────────────────────────────────────────────────
#pragma pack(push, 1)

struct Header {
  uint8_t msg_type;
  uint8_t _reserved{0};
  uint16_t msg_len;
  uint64_t clord_id;
  uint32_t sym;
};
static_assert(sizeof(Header) == 16);

// Client → Exchange
struct NewOrder {
  Header hdr;
  uint8_t side; // 0=Buy 1=Sell
  uint8_t _pad[3]{};
  int64_t px;
  int64_t qty;
};
static_assert(sizeof(NewOrder) == 36);

struct Cancel {
  Header hdr;
  uint64_t exch_id;
};
static_assert(sizeof(Cancel) == 24);

struct Replace {
  Header hdr;
  uint64_t exch_id;
  int64_t px;
  int64_t qty;
};
static_assert(sizeof(Replace) == 40);

// Exchange → Client
struct Ack {
  Header hdr;
  uint64_t exch_id;
};
static_assert(sizeof(Ack) == 24);

struct Fill {
  Header hdr;
  uint64_t exch_id;
  int64_t filled_qty;
  int64_t px;
};
static_assert(sizeof(Fill) == 40);

struct CancelAck {
  Header hdr;
  uint64_t exch_id;
};
static_assert(sizeof(CancelAck) == 24);

struct Reject {
  Header hdr;
  char reason[kRejectReasonLen];
};
static_assert(sizeof(Reject) == 48);

#pragma pack(pop)

} // namespace hft::oe::wire
