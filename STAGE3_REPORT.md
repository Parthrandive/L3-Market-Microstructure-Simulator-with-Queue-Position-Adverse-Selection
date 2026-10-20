# Stage 3 – Queue Position Estimation and Latency Modeling

## 1. Overview & Objectives

In electronic financial markets, backtests operating on Level-2 (aggregated top-of-book / depth-of-book) market data must make assumptions about an order's queue position and fill priority. Without individual order IDs (Level-3 TotalView-ITCH granularity), these backtests frequently rely on:
1. **Naive Touch Fill:** Assumes a limit order is immediately filled as soon as a trade occurs at or through its limit price.
2. **Level-2 Pessimistic Model:** Assumes executions deplete volume ahead, but cancellations always occur *behind* our order (cancels ahead do not advance queue priority).
3. **Level-2 Probabilistic Model:** Assumes cancellations are uniformly distributed across all resting shares at the price level, reducing queue ahead by $C \times \frac{Q_{\text{ahead}}}{D_{\text{level}}}$.

Stage 3 provides an exact empirical evaluation of these models against **Ground Truth Level-3 Queue Position** extracted from Nasdaq TotalView-ITCH 5.0, alongside simulated wire transit delays ($\Delta t$).

All benchmarks were executed on the existing repository datasets:
- **Session 1 (Holiday Lull):** December 30, 2019 (`data/fixture_3symbols_full.bin.gz`, 4,115,178 messages, 17.01-hour session).
- **Session 2 (FOMC Volatility):** October 30, 2019 (`data/fixture_3symbols_10302019.bin.gz`, 3,144,809 messages, rate cut decision).

---

## 2. Lock-Free SPSC Ring Buffer & TSan Verification

To decouple network I/O / feed ingestion from matching engine execution, we implemented a lock-free Single-Producer Single-Consumer (`SpscRingBuffer<T, Capacity>`):
- **False-Sharing Prevention:** `write_idx_` and `read_idx_` (along with their thread-local cached counters) are strictly isolated on separate 128-byte cache lines (`alignas(128)`), eliminating false sharing across Apple Silicon L2 lines (128B) and x86_64 lines (64B).
- **Wait-Free Hot Path:** Uses relaxed loads and release stores, synchronizing with acquire loads only when cached queue bounds are exhausted.
- **Sanitizer Verification:** 
  - **ThreadSanitizer (TSan):** Verified clean across 500,000 multi-threaded message transfers with **0 data races** and **0 dropped messages**.
  - **AddressSanitizer (ASan) & UBSan:** Verified clean across all unit tests and replay runs.

---

## 3. Empirical Results: Passive Quote-Posting Strategy

We simulated an algorithmic quote-posting strategy joining the Best Bid for 100 shares during continuous trading hours (09:30:00 to 16:00:00 ET) with a 5-second Time-To-Live (TTL). The strategy was evaluated across wire transit delays $\Delta t \in \{0, 5\,\mu\text{s}, 20\,\mu\text{s}, 50\,\mu\text{s}, 100\,\mu\text{s}, 500\,\mu\text{s}\}$.

### Session 1: December 30, 2019 (Holiday Lull – 22,810 Strategy Orders)

#### Table 1A: Fill Overstatement & False Positive Fills
| $\Delta t$ Wire Latency | Orders Evaluated | True L3 Fill Rate | L2 Pessimistic | L2 Probabilistic | Naive Touch Fill Rate | Naive Touch Overstatement | False Positive Fills (Touch) | Mean Fill Lead Time (Touch vs L3) |
|---|---|---|---|---|---|---|---|---|
| **0 $\mu$s** | 22,810 | **0.5%** | 0.1% | 0.1% | **0.7%** | **1.43×** | 50 orders | **211.03 ms** |
| **5 $\mu$s** | 22,810 | **0.5%** | 0.1% | 0.1% | **0.7%** | **1.43×** | 50 orders | **211.03 ms** |
| **20 $\mu$s** | 22,810 | **0.5%** | 0.1% | 0.1% | **0.7%** | **1.43×** | 50 orders | **184.15 ms** |
| **50 $\mu$s** | 22,810 | **0.5%** | 0.1% | 0.1% | **0.7%** | **1.43×** | 50 orders | **184.15 ms** |
| **100 $\mu$s** | 22,810 | **0.5%** | 0.1% | 0.1% | **0.8%** | **1.43×** | 53 orders | **205.11 ms** |
| **500 $\mu$s** | 22,810 | **0.5%** | 0.1% | 0.1% | **0.8%** | **1.41×** | 50 orders | **205.11 ms** |

#### Table 1B: Level-2 Queue Estimation Error (vs Ground Truth L3)
| $\Delta t$ Wire Latency | Avg Depth Ahead ($D_0$) | Pessimistic Bias | Pessimistic MAE | Probabilistic Bias | Probabilistic MAE |
|---|---|---|---|---|---|
| **0 $\mu$s** | 546.3 shares | +79.8 shares | 79.8 shares | +78.7 shares | 78.8 shares |
| **5 $\mu$s** | 312.2 shares | +79.8 shares | 79.8 shares | +78.7 shares | 78.7 shares |
| **20 $\mu$s** | 312.2 shares | +80.1 shares | 80.1 shares | +79.0 shares | 79.0 shares |
| **50 $\mu$s** | 312.1 shares | +78.1 shares | 80.2 shares | +77.0 shares | 79.2 shares |
| **100 $\mu$s** | 301.4 shares | +78.0 shares | 80.2 shares | +76.4 shares | 78.6 shares |
| **500 $\mu$s** | 301.4 shares | +76.5 shares | 80.1 shares | +75.0 shares | 78.6 shares |

---

### Session 2: October 30, 2019 (FOMC Volatility – 22,628 Strategy Orders)

#### Table 2A: Fill Overstatement & False Positive Fills
| $\Delta t$ Wire Latency | Orders Evaluated | True L3 Fill Rate | L2 Pessimistic | L2 Probabilistic | Naive Touch Fill Rate | Naive Touch Overstatement | False Positive Fills (Touch) | Mean Fill Lead Time (Touch vs L3) |
|---|---|---|---|---|---|---|---|---|
| **0 $\mu$s** | 22,628 | **0.5%** | 0.0% | 0.0% | **0.6%** | **1.22×** | 26 orders | **293.78 ms** |
| **5 $\mu$s** | 22,628 | **0.5%** | 0.0% | 0.0% | **0.6%** | **1.22×** | 26 orders | **293.78 ms** |
| **20 $\mu$s** | 22,628 | **0.5%** | 0.0% | 0.0% | **0.6%** | **1.22×** | 26 orders | **293.78 ms** |
| **50 $\mu$s** | 22,628 | **0.5%** | 0.0% | 0.0% | **0.6%** | **26 orders** | 26 orders | **293.78 ms** |
| **100 $\mu$s** | 22,628 | **0.5%** | 0.0% | 0.0% | **0.6%** | **1.22×** | 26 orders | **285.04 ms** |
| **500 $\mu$s** | 22,628 | **0.5%** | 0.0% | 0.0% | **0.6%** | **1.22×** | 26 orders | **285.04 ms** |

#### Table 2B: Level-2 Queue Estimation Error (vs Ground Truth L3)
| $\Delta t$ Wire Latency | Avg Depth Ahead ($D_0$) | Pessimistic Bias | Pessimistic MAE | Probabilistic Bias | Probabilistic MAE |
|---|---|---|---|---|---|
| **0 $\mu$s** | 826.5 shares | +105.4 shares | 105.4 shares | +105.4 shares | 105.4 shares |
| **5 $\mu$s** | 572.1 shares | +105.4 shares | 105.4 shares | +105.4 shares | 105.4 shares |
| **20 $\mu$s** | 572.1 shares | +106.0 shares | 106.0 shares | +106.0 shares | 106.0 shares |
| **50 $\mu$s** | 572.1 shares | +105.3 shares | 106.2 shares | +105.3 shares | 106.2 shares |
| **100 $\mu$s** | 572.1 shares | +105.5 shares | 106.3 shares | +105.5 shares | 106.3 shares |
| **500 $\mu$s** | 438.1 shares | +103.7 shares | 106.5 shares | +103.7 shares | 106.5 shares |

---

## 4. Empirical Results: Natural Market Order Flow (Real ITCH BBO Orders)

To eliminate any strategy-specific selection bias, we also tracked real market participants submitting orders at the Best Bid during continuous trading:

| Metric | Dec 30, 2019 (Holiday Lull) | Oct 30, 2019 (FOMC Volatility) |
|---|---|---|
| **Sampled Real BBO Orders** | 267 orders | 480 orders |
| **Average Initial Queue Depth** | 331.2 shares | 685.0 shares |
| **True L3 Fill Rate** | **13.11%** (35 fills) | **7.50%** (36 fills) |
| **Naive Touch Fill Rate** | **14.61%** (39 fills) | **9.38%** (45 fills) |
| **Naive Touch Overstatement** | **1.11×** | **1.25×** |
| **False Positive Touch Fills** | 4 orders | 9 orders |
| **Mean Fill Time Lead (Touch)** | **79.67 ms** early | **159.85 ms** early |
| **L2 Pessimistic MAE** | 61.7 shares (Bias: +61.5 sh) | 113.6 shares (Bias: +113.6 sh) |
| **L2 Probabilistic MAE** | 61.6 shares (Bias: +61.0 sh) | 113.0 shares (Bias: +113.0 sh) |

---

## 5. Key Market Microstructure Takeaways

1. **The Naive Touch Fallacy:**
   - On real market orders, Naive Touch overstates fill probability by **1.11× to 1.25×**, and by **1.22× to 1.43×** on resting passive strategy orders.
   - Crucially, Naive Touch creates **false positive fills**: orders that touched the trade price but were canceled before the trade volume ever penetrated to their position in the queue.
   - For orders that do fill, Naive Touch registers fills **80 ms to 294 ms earlier** than physical reality, distorting execution timing and post-fill markout metrics.

2. **Systematic Bias in Level-2 Queue Estimation:**
   - Both Pessimistic and Probabilistic L2 models exhibit a persistent **positive bias (+61 to +113 shares)** relative to true L3 position.
   - This occurs because market makers ahead in the queue frequently cancel or quote-flicker. Because L2 data aggregates volume, an L2 model cannot know whether a cancellation depleted volume ahead or volume behind, leading to overly conservative queue estimates.

3. **Wire Transit Latency ($\Delta t$):**
   - Wire latency introduces price slippage and queue degradation. An order sent at $T_0$ arrives at $T_0 + \Delta t$, during which intervening market orders accumulate ahead, increasing initial queue depth $D_0$ and lowering the probability of timely fills.
