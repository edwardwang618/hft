# HFT Demo

End-to-end HFT system: a scripted exchange broadcasts market data (binary and
FIX) over UDP while accepting orders over TCP, and a trading client processes
the full pipeline from raw bytes to exchange fills.

---

## Quick start

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)

# Terminal 1 — exchange (waits for connection before broadcasting)
./build/tools/exchange_server 127.0.0.1 12345 12346 0.0.0.0 9001

# Terminal 2 — trading client
./build/src/hft 0.0.0.0 12345 127.0.0.1 9001          # binary feed
./build/src/hft 0.0.0.0 12346 127.0.0.1 9001 --fix    # FIX feed
```

### Custom scenario (CSV)

The exchange reads MD events from a CSV instead of the built-in 8-second script:

```bash
./build/tools/exchange_server 127.0.0.1 12345 12346 0.0.0.0 9001 \
    tools/scenarios/default.csv
```

```
# delay_ms, type,   sym, id, side,  price,  qty
  0,        Add,    1,   1,  Sell,  101.00, 1000
  0,        Add,    1,   2,  Buy,    99.00, 1000
  2000,     Cancel, 1,   1
  2000,     Add,    1,   3,  Sell,  100.00, 1000
  4000,     Cancel, 1,   2
  4000,     Add,    1,   4,  Buy,   100.00, 1000
```

`delay_ms` is relative to when the OE connection is accepted.  
`price` is decimal (e.g. `101.00`); converted internally via `kPriceScale`.

---

## Tests

```bash
ctest --test-dir build -j$(nproc)   # 219 tests
```

---

## Client architecture

The sections below describe the `hft` binary only (the trading client).

### System context

```
┌─────────────────────────────────────┐         ┌──────────────────────────────────────┐
│          exchange_server            │         │                hft                   │
│                                     │         │                                      │
│  tools/scenarios/*.csv              │         │  Core 1 ─ SCHED_IDLE                 │
│  (other participants' orders)       │         │  ┌────────────┐                      │
│          │                          │         │  │ UdpReceiver│                      │
│  NetworkedExchange                  │         │  │ recv_batch │                      │
│    BinarySerializer ─► UDP :12345 ──┼─────────┼─►│            │                      │
│    FixSerializer    ─► UDP :12346 ──┼─────────┼─►│            │                      │
│                                     │         │  └─────┬──────┘                      │
│    Matching engine  ◄── TCP :9001 ──┼─────────┼───┐   │ SpscSink<1024>               │
│                     ──► TCP :9001 ──┼─────────┼───┘   │ (lock-free ring)             │
└─────────────────────────────────────┘         │        │                             │
                                                │  Core 2 ─ SCHED_FIFO                │
                                                │  ┌──────▼──────────────────────┐    │
                                                │  │    MD pipeline               │    │
                                                │  │    Gateway chain             │    │
                                                │  └─────────────────────────────┘    │
                                                └──────────────────────────────────────┘
```

---

### Threading model

The client runs two threads pinned to separate cores:

```
Core 1  (SCHED_IDLE)           Core 2  (SCHED_FIFO)
─────────────────────          ─────────────────────────────────────────────
UdpReceiver::recv_batch()
  recvmmsg() batch syscall
  └─► SpscSink::on_bytes()            SpscSink::try_pop()
        push packet to ring   ──────► drain all packets
                                        └─► FeedHandler::on_bytes()
                                              └─► ... full MD pipeline ...
                                                    └─► SimpleQuoter
                                                          └─► gateway chain
                                              OrderGatewayT::poll_reports()
                                                TcpSession::recv()
                                                └─► OeDecoder (Ack/Fill/Reject)
```

`io_thread` uses `SCHED_IDLE` so the OS never preempts the hot-path.  
`hot_thread` uses `SCHED_FIFO` for deterministic latency (non-fatal if no `CAP_SYS_NICE`).

**SpscSink** (`include/hft/exchange/spsc_sink.hpp`) is the only inter-thread
data structure. It wraps a `SpscRing<Packet<2048>, 1024>` — acquire/release
atomics on separate cache lines for the head and tail; no mutex, no CAS loop.

---

### MD pipeline — mixin composition

The pipeline is built entirely from template parameters.  
There are **no virtual calls** in the hot path; the compiler sees the full
chain and inlines across every stage boundary.

#### Type structure

```
FeedHandler < Next  =  GapDetector < Next  =  BookBuilder < Next  = NullNext,
                                                             Book  = MapOrderBook<SimpleQuoter> > >,
              Parser = BinaryParser | FixParser >
```

Each stage holds a **reference** to the next stage (no ownership, no heap).
Swapping a stage costs a single template argument change — the rest recompiles
with full inlining for the new composition.

#### Data flow

```
UdpReceiver / SpscSink
    │  raw bytes (one UDP packet per call)
    ▼
FeedHandler<Next, Parser>                       feed_handler.hpp
    │  accumulates bytes in buf_
    │  Parser::parse(span) → ParseResult{consumed, MdEvent, seq}
    │  loops until NeedMore
    ▼
GapDetector<Next>                               gap_detector.hpp
    │  seq < expected  →  duplicate, drop
    │  seq == expected →  deliver + drain reorder buffer
    │  seq >  expected →  buffer + fire on_gap(from, to) callback
    ▼
BookBuilder<Next, Book>                         book_builder.hpp
    │  books_[sym] created on first sight (factory lambda)
    │  routes every MdEvent to the correct per-symbol book
    │  after apply(): forwards (seq, event, books) to Next (NullNext here)
    ▼
MapOrderBook<Listener>   (one instance per symbol)     map_order_book.hpp
    │  CRTP: OrderBookBase<MapOrderBook, Listener>
    │  bids_: std::map<Price, Level>  (descending via std::greater)
    │  asks_: std::map<Price, Level>  (ascending)
    │  on Add/Cancel/Reduce/Exec/Replace/Trade → update levels
    │  whenever BBO changes → Listener::on_bbo(sym, bid, ask)
    ▼
SimpleQuoter::on_bbo(sym, bid_px, ask_px)       simple_quoter.hpp
    │  computes quote prices and sizes
    │  cancel stale orders, send new quotes
    ▼
    IOrderGateway::send_new(sym, side, px, qty)
```

#### Parser is also a policy

```
FeedHandler<..., BinaryParser>   binary framing: fixed-size structs, magic header
FeedHandler<..., FixParser>      FIX ASCII:      \n-terminated tag=value pairs
```

Switching protocol is one template argument; the rest of the pipeline is
identical. `main()` uses `if (use_fix) run<FixParser>() else run<BinaryParser>()`.

---

### Order book — CRTP

```
OrderBookBase<Derived, Listener>              order_book_base.hpp
    CRTP base: apply() dispatch, Listener callbacks, BBO tracking
    Derived must implement: rest_(), snapshot_bbo(), price_valid_(),
                            cancel(), reduce(), execute(), clear_state_()
         │
         ├── MapOrderBook<Listener>           map_order_book.hpp
         │       std::map per side — O(log n) insert/cancel, unbounded price
         │       used in production path (strategy attached as Listener)
         │
         └── ArrayOrderBook<Listener>         array_order_book.hpp
                 fixed-size array per side — O(1) by tick index, bounded price
                 used in benchmarks / tests with known tick range
```

---

### Gateway chain

The gateway chain sits between the strategy and the TCP wire. It uses
**virtual dispatch** (`IOrderGateway` interface) — latency here is acceptable
because order submission is rare relative to the MD update rate.

```
SimpleQuoter
    │  IOrderGateway::send_new / send_cancel / send_replace
    ▼
DeferredGateway                                 (in main.cpp)
    │  thin stub; real_ pointer set by bind() after full construction
    │  ── solves the circular construction dependency (see below) ──
    ▼
RiskGateway  :  IOrderGateway                   risk_gateway.hpp
    │  check_kill_switch()    atomic flag, survives any thread
    │  check_qty()            order qty ≤ max_order_qty
    │  check_notional()       px × qty ≤ max_order_notional  (overflow-safe)
    │  check_rate()           RateLimiter token bucket
    │  check_fat_finger()     |px − mid| / mid ≤ fat_finger_bps
    │  check_position()       |net_pos + qty| ≤ max_position_per_sym
    │  any failure → ++rejected_<kind>, return kInvalidOrderId
    ▼
OrderGatewayT<Session>  :  IOrderGateway        order_gateway.hpp
    │  OeEncoder::encode_new/cancel/replace → bytes into stack buffer
    │  Session::send_all(bytes) → network
    │  (Session = TcpSession in prod, InProcessSession in tests)
    ▼
TcpSession                                      tcp_session.hpp
    │  blocking send_all, non-blocking recv
    │  TCP wire to exchange OE port
    ▼
exchange_server  (OE port)
```

**Execution reports** flow in the reverse direction:

```
TcpSession::recv()  ◄──  TCP wire  ◄──  exchange_server
    │
OeDecoder::on_bytes()   (called from poll_reports() on the hot thread)
    │
IOrderListener::on_ack / on_fill / on_cancel_ack / on_reject
    │
DiagListener  (prints to stderr in this demo)
    │  production: OrderManager routes fills back to strategy
```

---

### DeferredGateway — breaking the construction cycle

Strategy and the gateway chain have a circular dependency at construction time:

```
SimpleQuoter  needs  IOrderGateway  at construction
BookBuilder   needs  SimpleQuoter   (book factory captures reference)
OrderGatewayT needs  TcpSession     (independent)
RiskGateway   needs  OrderGatewayT  (independent)
                ↑
          needs to exist before BookBuilder
```

`DeferredGateway` resolves this by acting as a stub:

```
1. Construct DeferredGateway (stub, real_ = nullptr)
2. Construct SimpleQuoter(deferred_gw)
3. Construct BookBuilder(lambda capturing quoter)
4. Construct RiskGateway, OrderGatewayT, TcpSession   (independent)
5. deferred_gw.bind(risk_gw)                          (wire it up)
6. oe_session.connect()                               (go live)
```

---

### Key interfaces

| Interface | Location | Purpose |
|-----------|----------|---------|
| `IOrderGateway` | `strategy/i_order_gateway.hpp` | `send_new / send_cancel / send_replace` |
| `IOrderListener` | `strategy/i_order_listener.hpp` | `on_ack / on_fill / on_cancel_ack / on_reject` |
| `IByteSink` | `exchange/byte_sink.hpp` | `on_bytes(data, len)` — used by SpscSink and InProcessSession |
| `IWireSerializer` | `exchange/wire_serializer.hpp` | `encode(MdEvent, seq, buf)` — binary and FIX impls |
| `IParser` | `md/parser.hpp` | `parse(span) → ParseResult` — binary and FIX impls |

Virtual dispatch appears only on `IOrderGateway` / `IOrderListener` (order
path) and `IByteSink` / `IWireSerializer` (exchange side).  
The entire MD hot path (`FeedHandler` → `MapOrderBook`) is virtual-free.

---

### File map

```
include/hft/
├── core/
│   ├── types.hpp              Price, Qty, OrderId, Side, kPriceScale
│   ├── order_book_base.hpp    CRTP base: apply() dispatch, BBO callbacks
│   ├── map_order_book.hpp     std::map-backed book (production)
│   ├── array_order_book.hpp   array-backed book (benchmarks)
│   ├── spsc_ring.hpp          lock-free SPSC queue
│   └── thread_util.hpp        pin_to_core, set_realtime_priority
├── md/
│   ├── md_event.hpp           MdEvent variant (Add/Cancel/Reduce/Exec/Replace/Trade/Clear)
│   ├── binary_parser.hpp      fixed-struct framing parser
│   ├── fix_parser.hpp         FIX ASCII tag=value parser
│   ├── wire_binary.hpp        binary wire format constants
│   └── wire_fix.hpp           FIX tag constants
├── pipeline/
│   ├── feed_handler.hpp       bytes → MdEvent (owns Parser, calls Next)
│   ├── gap_detector.hpp       seq reordering + gap callbacks
│   └── book_builder.hpp       per-symbol book dispatch + NullNext
├── exchange/
│   ├── spsc_sink.hpp          IByteSink → SpscRing (io→hot bridge)
│   ├── binary_serializer.hpp  MdEvent → binary bytes
│   ├── fix_serializer.hpp     MdEvent → FIX ASCII bytes
│   ├── csv_md_player.hpp      CSV → vector<TimedEvent>
│   ├── networked_exchange.hpp exchange server class (UDP+TCP)
│   └── in_process_session.hpp IByteSink session for integration tests
├── oe/
│   ├── wire_oe.hpp            OE wire structs (NewOrder/Ack/Fill/...)
│   ├── oe_encoder.hpp         NewOrder/Cancel/Replace → bytes
│   ├── oe_decoder.hpp         bytes → IOrderListener callbacks
│   ├── order_gateway.hpp      IOrderGateway → encoder + Session
│   └── tcp_session.hpp        blocking TCP send, non-blocking recv
├── risk/
│   ├── risk_limits.hpp        RiskLimits struct
│   ├── rate_limiter.hpp       token-bucket order rate limiter
│   └── risk_gateway.hpp       pre-trade checks, kill switch
└── strategy/
    ├── i_order_gateway.hpp    send_new / send_cancel / send_replace
    ├── i_order_listener.hpp   on_ack / on_fill / on_cancel_ack / on_reject
    ├── simple_quoter.hpp      two-sided market maker (demo strategy)
    └── order_manager.hpp      clord_id ↔ exch_id tracking, fill routing
```
