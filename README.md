# HFT Demo

End-to-end HFT pipeline: scripted exchange broadcasts market data (binary and
FIX) over UDP, trading system processes `FeedHandler → GapDetector →
BookBuilder → SimpleQuoter → RiskGateway`, and sends orders back over TCP.

## Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

## Running the demo

The exchange waits for the trading system's OE (TCP) connection before
broadcasting the initial book, so **start the exchange server first**, then the
trading system in a second terminal.

### Binary feed (default)

**Terminal 1 — exchange:**
```bash
./build/tools/exchange_server 127.0.0.1 12345 12346 0.0.0.0 9001
#                              md_dest   ^bin  ^fix  oe_addr oe_port
```

**Terminal 2 — trading system:**
```bash
./build/src/hft 0.0.0.0 12345 127.0.0.1 9001
#               md_addr  ^port oe_host   oe_port
```

### FIX feed

Same exchange server (it broadcasts both simultaneously). Pass `--fix` to the
trading system to switch parsers:

**Terminal 2 — trading system (FIX):**
```bash
./build/src/hft 0.0.0.0 12346 127.0.0.1 9001 --fix
#                        ^FIX port              ^parser
```

### Custom scenario (CSV)

Pass a CSV file as the 6th argument to drive the exchange from data instead
of the built-in script:

```bash
./build/tools/exchange_server 127.0.0.1 12345 12346 0.0.0.0 9001 \
    tools/scenarios/default.csv
```

CSV format — one event per line, `#` for comments:
```
# delay_ms, type,   sym, id, side,  price,  qty
  0,        Add,    1,   1,  Sell,  101.00, 1000
  0,        Add,    1,   2,  Buy,    99.00, 1000
  2000,     Cancel, 1,   1
  2000,     Add,    1,   3,  Sell,  100.00, 1000
  4000,     Cancel, 1,   2
  4000,     Add,    1,   4,  Buy,   100.00, 1000
```

- `delay_ms`: milliseconds after the OE connection is accepted
- `type`: `Add` | `Cancel` | `Reduce` | `Clear`
- `price`: decimal (e.g. `101.00`), converted internally via `kPriceScale`

The exchange's matching engine (Ack / Fill) is always active regardless of
the CSV — the CSV only controls what the exchange *broadcasts* as market data
from other participants.

### Disable one feed

Set `fix_port` to `0` to broadcast binary only:
```bash
./build/tools/exchange_server 127.0.0.1 12345 0 0.0.0.0 9001
```

## Scenario timeline

| Time | Exchange broadcasts | Trading system |
|------|---------------------|----------------|
| T+0s | `Add Sell@101` + `Add Buy@99` (seed book) | receives initial BBO, quotes |
| T+2s | `Cancel ask@101` + `Add Sell@100` | ask tightens, strategy re-quotes |
| T+4s | `Cancel bid@99` + `Add Buy@100` | locked market — strategy bid crosses ask → **Fill** |
| T+8s | shutdown | — |

## Expected output

**Exchange (stderr):**
```
INFO:  MD(binary) → 127.0.0.1:12345
INFO:  MD(FIX)    → 127.0.0.1:12346
INFO:  OE listen   0.0.0.0:9001
INFO:  waiting for OE connection...
INFO:  client connected
INFO:  initial book — bid=99.0000  ask=101.0000
INFO:  [T+2s] ask tightens → 100.0000
INFO:  [T+4s] bid tightens → 100.0000  (locked market)
[NEW ]  clord=1  sym=1  BUY  px=100.0000  qty=1
[ACK ]  clord=1  exch=1
[FILL]  clord=1  exch=1  qty=1  px=1000000
INFO:  [T+8s] scenario complete — shutting down

--- exchange stats ---
  orders received : 1
  fills sent      : 1
  MD frames sent  : 8
```

**Trading system (stderr):**
```
INFO:  OE connected to 127.0.0.1:9001
INFO:  MD listening on 0.0.0.0:12345  [binary]
[ACK]    clord=1  exch=1
[FILL]   exch=1  qty=1  px=1000000

--- shutdown stats ---
  md  msgs_parsed  : 8
  md  corrupt      : 0
  spsc dropped     : 0
  risk rejected    : kill=0 qty=0 notional=0 rate=0 fat_finger=0 position=0
```

## Architecture

```
exchange_server                         hft
───────────────                         ───
UDP(binary) ──────────────────────────► UdpReceiver
UDP(FIX)    ──────────────────────────► UdpReceiver (--fix)
                                            │
                                        SpscSink (SPSC ring, io_thread → hot_thread)
                                            │
                                        FeedHandler<BinaryParser|FixParser>
                                            │
                                        GapDetector
                                            │
                                        BookBuilder → MapOrderBook
                                            │
                                        SimpleQuoter
                                            │
                                        RiskGateway (limits check)
                                            │
TCP(OE) ◄─────────────────────────────── OrderGateway → TcpSession
TCP(OE) ──────────────────────────────► poll_reports() → Ack / Fill callbacks
```

## Tests

```bash
ctest --test-dir build --output-on-failure -j$(nproc)
```
