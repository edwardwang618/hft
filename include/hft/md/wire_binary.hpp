#pragma once
#include <cstdint>

// ============================================================================
// HFT Binary Market Data Wire Format v1
//
// Little-endian, packed (no alignment padding anywhere).
// Every message = Header (4 B) + Body (variable).
//
// Header:
//   u8  msg_type
//   u8  _reserved  (must be 0)
//   u16 msg_len    (whole message length incl. header)
//
// Body layouts (field order MUST match md_event.hpp structs):
//   Add     : ts:u64 sym:u32 id:u64 side:u8 px:i64 qty:i64              (37 B)
//   Cancel  : ts:u64 sym:u32 id:u64                                     (20 B)
//   Reduce  : ts:u64 sym:u32 id:u64 new_qty:i64                         (28 B)
//   Exec    : ts:u64 sym:u32 id:u64 exec_qty:i64 px:i64 trade_id:u64    (44 B)
//   Replace : ts:u64 sym:u32 old_id:u64 new_id:u64 side:u8 px:i64 qty:i64 (45 B)
//   Trade   : ts:u64 sym:u32 px:i64 qty:i64 trade_id:u64                (36 B)
//   Clear   : ts:u64 sym:u32                                            (12 B)
// ============================================================================

namespace hft::md::wire {

enum MsgType : uint8_t {
  kAdd = 1,
  kCancel = 2,
  kReduce = 3,
  kExec = 4,
  kReplace = 5,
  kTrade = 6,
  kClear = 7,
};

#pragma pack(push, 1)
struct Header {
  uint8_t msg_type;
  uint8_t _reserved;
  uint16_t msg_len;
};
static_assert(sizeof(Header) == 4);
#pragma pack(pop)

} // namespace hft::md::wire