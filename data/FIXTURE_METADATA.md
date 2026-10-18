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

## 3. Full-Day Fixture Pipeline

The full day feed (`12302019.NASDAQ_ITCH50.gz`, 3.52 GB compressed, ~250M messages) is processed using:
1. `python3 scripts/download_parallel.py --workers 40`: Downloads the multi-gigabyte feed using parallel HTTP Range requests to bypass single-connection server throttling.
2. `python3 scripts/cut_fixture.py --source data/raw/12302019.NASDAQ_ITCH50.gz --output data/fixture_3symbols_full.bin.gz`: Filters all 250M messages to extract full-day order books for AAPL, SPY, NVDA.
3. Full-day artifacts are excluded from Git via `.gitignore` (`data/raw/` and `data/*full*`).
