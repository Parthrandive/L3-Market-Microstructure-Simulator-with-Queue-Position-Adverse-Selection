# ITCH 5.0 Fixture Metadata & Verification

## 1. Unit Test Slice Fixture (`data/fixture_3symbols.bin.gz`)

- **Source Date:** December 30, 2019 (`12302019.NASDAQ_ITCH50.gz`)
- **File Size:** 169,250 bytes (compressed)
- **SHA-256 Checksum:** `7827f15841c9c1555a1455cf03d652ad09c58f40e7db12327e4098d3b52d981a`
- **Total Valid Messages:** 16,869
- **Time Span:**
  - **First Message:** Type `S` at `03:04:32.058` (`11,072,057,543,747` ns since midnight)
  - **Last Message:** Type `D` at `06:57:59.781` (`25,079,781,472,444` ns since midnight)
  - **Duration:** 14,007.72 seconds (233.46 minutes)
  - **Phase:** Pre-market early session (3:04 AM – 6:58 AM ET). Regular market hours begin at 9:30 AM (34.2 μs). This explains why activity is dominated by SPY ETF rebalancing and why AAPL/NVDA volume ramps up later in the day.

### Symbol & Locate Breakdown
- **AAPL** (Locate 13): 3,380 messages
- **NVDA** (Locate 5708): 111 messages
- **SPY**  (Locate 7451): 13,376 messages
- **System Events** (Locate 0): 2 messages

### Message Type Breakdown
| Type | Description | Count |
| :--- | :--- | :--- |
| `A` | Add Order (no MPID) | 7,973 |
| `D` | Order Delete | 7,759 |
| `U` | Order Replace | 416 |
| `X` | Order Cancel | 384 |
| `E` | Order Executed | 159 |
| `L` | Market Participant Position | 139 |
| `P` | Non-Cross Trade | 28 |
| `R` | Stock Directory | 3 |
| `H` | Stock Trading Action | 3 |
| `Y` | Reg SHO Short Sale Price Test | 3 |
| `S` | System Event | 2 |
| **Total** | | **16,869** |

---

## 2. Independent Verification Tooling

To ensure verification integrity, [`scripts/count_raw_types.py`](../scripts/count_raw_types.py) was implemented completely independently from the cutter script:
- Zero shared imports, functions, or data structures with `cut_fixture.py`.
- Parses raw 2-byte big-endian framed ITCH streams directly.
- Both scripts verify exact message count parity (`16,869` messages).

---

## 3. Full-Day Fixture (`data/fixture_3symbols_full.bin.gz`)

- **Source Date:** December 30, 2019 (`12302019.NASDAQ_ITCH50.gz`, 3.52 GB compressed)
- **Scanned Messages:** 268,744,780 messages
- **File Size:** 41,272,679 bytes (~41.2 MB compressed, excluded from git via `.gitignore`)
- **SHA-256 Checksum:** `12a66b51ecdddcfa85a005d022d05afd354cf871cb52228c6f6303cd4158a1f0`
- **Total Kept Messages:** 4,115,178
- **Time Span:**
  - **First Message:** Type `S` at `03:04:32.058` (`11,072,057,543,747` ns since midnight)
  - **Last Message:** Type `S` at `20:05:00.000` (`72,300,000,040,070` ns since midnight)
  - **Total Span:** 17.01 hours (full trading day: pre-market, 09:30 open, regular hours, 16:00 close, and post-market)

### Symbol Breakdown
- **SPY**  (Locate 7451): 2,138,932 messages
- **AAPL** (Locate 13):   1,519,865 messages
- **NVDA** (Locate 5708):   456,375 messages
- **System Events** (Locate 0): 6 messages

### Message Type Breakdown
| Type | Description | Count |
| :--- | :--- | :--- |
| `A` | Add Order (no MPID) | 1,887,549 |
| `D` | Order Delete | 1,803,187 |
| `U` | Order Replace | 256,528 |
| `E` | Order Executed | 120,437 |
| `X` | Order Cancel | 21,391 |
| `P` | Non-Cross Trade | 17,481 |
| `F` | Add Order with MPID | 5,170 |
| `C` | Order Executed with Price | 1,923 |
| `I` | NOII Imbalance Message | 1,352 |
| `L` | Market Participant Position | 139 |
| `S` | System Event | 6 |
| `Q` | Cross Trade | 6 |
| `R` | Stock Directory | 3 |
| `H` | Stock Trading Action | 3 |
| `Y` | Reg SHO Short Sale Test | 3 |
| **Total** | | **4,115,178** |

Independently verified against raw stream byte framing using `scripts/count_raw_types.py`.
