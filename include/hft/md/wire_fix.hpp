#pragma once

// ============================================================================
// HFT FIX-like Market Data Wire Format v1
//
// ASCII, line-delimited. One message per line, '\n' terminator.
// Fields separated by '|', each field is "tag=value".
// Tag order is NOT significant. Unknown tags are ignored.
// No BeginString / BodyLength / CheckSum (this is NOT real FIX).
//
// MsgType (tag 35):
//   A=Add  X=Cancel  R=Reduce  E=Exec  G=Replace  T=Trade  C=Clear
//
// Per-message required tags:
//   Add     : 35,34,52,55, 37,54,44,38
//   Cancel  : 35,34,52,55, 37
//   Reduce  : 35,34,52,55, 37,39
//   Exec    : 35,34,52,55, 37,32,44,17
//   Replace : 35,34,52,55, 41,37,54,44,38     (41=old_id, 37=new_id)
//   Trade   : 35,34,52,55, 44,38,17
//   Clear   : 35,34,52,55
// ============================================================================

namespace hft::md::wire_fix {

// MsgType characters (value of tag 35)
constexpr char kAdd = 'A';
constexpr char kCancel = 'X';
constexpr char kReduce = 'R';
constexpr char kExec = 'E';
constexpr char kReplace = 'G';
constexpr char kTrade = 'T';
constexpr char kClear = 'C';

// Tag numbers
constexpr int kTagMsgType = 35;
constexpr int kTagSeqNum = 34;
constexpr int kTagTs = 52;
constexpr int kTagSymbol = 55;
constexpr int kTagOrderId = 37;
constexpr int kTagOrigOrderId = 41;
constexpr int kTagSide = 54;
constexpr int kTagPrice = 44;
constexpr int kTagQty = 38;
constexpr int kTagLastQty = 32;
constexpr int kTagNewQty = 39;
constexpr int kTagTradeId = 17;

// Side (tag 54) values
constexpr int kSideBuy = 1;
constexpr int kSideSell = 2;

constexpr char kFieldSep = '|';
constexpr char kFrameEnd = '\n';

} // namespace hft::md::wire_fix