# Stage 4: Fill Simulator, Queue-Aware Priority & Adverse Selection Analysis

**Document Status:** Publication-Grade Experimental Report  
**Author:** Parth Randive  
**Environment:** Apple Silicon (macOS, Apple M1, 24 MHz hardware timebase), Apple Clang C++20  
**Test Datasets:** Nasdaq TotalView-ITCH 5.0  
- **Dec 30, 2019:** Holiday Lull Session (Low Volatility, 4,115,178 raw messages, 64,711 regular-hours strategy orders)  
- **Oct 30, 2019:** FOMC Rate Decision Session (High Volatility, 3,115,960 raw messages, 63,218 regular-hours strategy orders)  
**Primary Baseline Strategy:** Passive Buy 100 shares @ Best Bid when Touch Imbalance $\text{OBI} > 0.0$, 5.0s TTL, sampled at 250 ms intervals strictly during Regular Trading Hours (09:30:00 to 16:00:00 ET).

---

## 1. Executive Summary & Definitions

Commercial backtesters and academic literature frequently simulate limit order execution using **Naive Touch Fill**:
> **Definition (Naive Touch Fill):** An execution simulation model that assumes a resting limit order fills immediately and in full upon the occurrence of any trade at or through its limit price, disregarding its FIFO queue priority, resting depth ahead, and market impact.

This report presents empirical results from **Stage 4** of our L3 market microstructure simulation engine. We directly compare three fill models on identical strategy opportunities across low-volatility and high-volatility trading days:
1. **Model 1: Naive Touch Fill** (zero-depth touch assumption).
2. **Model 2: Queue-Aware L3 Fill** ($\Delta t = 0\,\mu\text{s}$, true intrusive FIFO queue depletion).
3. **Model 3: Latency-Aware L3 Fill** ($\Delta t \in \{50\,\mu\text{s}, 100\,\mu\text{s}, 250\,\mu\text{s}, 500\,\mu\text{s}\}$, wire transit delay before joining book).

### Key Headline Findings:
1. **Significant Fill Rate Overstatement (1.77× to 1.82×):**
   - On Dec 30 (holiday lull), Naive Touch yields a **37.50% [95% CI: 35.86% – 39.13%]** fill rate, whereas true Queue L3 achieves **20.57% [95% CI: 18.89% – 22.41%]**—an overstatement ratio of **1.82× [95% CI: 1.74× – 1.92×]**.
   - On Oct 30 (FOMC volatile), Naive Touch yields a **37.57% [95% CI: 35.53% – 39.53%]** fill rate versus **21.21% [95% CI: 19.39% – 22.94%]** under Queue L3—an overstatement of **1.77× [95% CI: 1.71× – 1.85×]**.
2. **Artificial Premature Execution Lead Time (~318 to 369 ms):**
   - For orders that fill under both models, Naive Touch triggers **317.5 ms [95% CI: 293.0 – 344.3 ms]** earlier on Dec 30, and **368.6 ms [95% CI: 333.5 – 403.3 ms]** earlier on Oct 30. Naive backtests exit trades and calculate markouts before the real order would have ever reached the front of the queue.
3. **Adverse Selection & Spread Degradation at Short Horizons (5s):**
   - In Queue L3 execution, an order fills **only when executions consume the queue ahead of it** (the resting shares ahead plus the order's own 100 shares). Later arrivals join *behind* our order and do not impede our execution. Reaching our position requires significant selling into the bid level.
   - Consequently, true L3 fills suffer adverse selection at the 5s horizon: markout is **+0.09 bps [0.02 .. 0.16]** on Dec 30, compared to **+0.16 bps [0.12 .. 0.20]** under Naive Touch (which fills on benign non-depleting trades). Naive touch overstates 5s mark-to-mid markout PnL by **3.19× [95% CI: 2.17× – 10.55×]** on Dec 30 and **2.07× [95% CI: 1.81× – 2.46×]** on Oct 30.
4. **Convergence to Statistical Noise at 30s (No Significant Inversion):**
   - At the 30-second horizon, net mark-to-mid markouts for both models converge toward zero (+0.01 bps [−0.26, 0.25] for touch vs −0.05 bps [−0.36, 0.24] for queue on Dec 30; +0.09 bps vs +0.16 bps on Oct 30). Because both 95% bootstrap intervals span zero and the Dec 30 difference is only 0.06 bps (pure statistical noise), while 30s PnL on Oct 30 is virtually identical ($5,794 vs $5,745), we report **no statistically significant 30-second sign inversion**.
5. **Wire Latency Null Result:**
   - Across wire transit latencies $\Delta t \in [0, 500\,\mu\text{s}]$, fill rates decline marginally (from 20.57% to 20.43% on Dec 30; from 21.21% to 20.95% on Oct 30), and 30s mark-to-mid markout PnL changes by less than 2%, with overlapping 95% bootstrap confidence intervals. For a passive 5-second resting order in liquid US equities, sub-millisecond wire latency has a statistically negligible effect compared to the first-order impact of queue priority.

---

## 2. Cross-Stage Technical Reconciliation (Stage 3 vs. Stage 4)

In an early prototype draft of Stage 3 (commit `81dc4fb`), a fill rate of ~0.5% was initially reported. In Stage 4, the measured true fill rate is ~20.6% on Dec 30 and ~21.2% on Oct 30. The 40× gap between the early prototype and the final pipeline is fully resolved as follows:

1. **Root Cause: Vector Reallocation & Dangling Pointer Bug in Prototype:**
   - In `81dc4fb`, simulated orders were stored in `std::vector<SimOrder> completed_orders`, while active orders were tracked via raw pointers `std::vector<SimOrder*> resting_sim_orders`.
   - As thousands of orders were pushed throughout the trading day, `completed_orders` exceeded its capacity and reallocated. This invalidated all previously stored pointers in `resting_sim_orders`.
   - When execution messages arrived, the engine iterated through dangling pointers to freed memory. As a result, `ord->l3_filled = true` was silently written to stale heap memory for over 98% of orders, artificially suppressing detected fills to ~113 out of 22,628 orders (~0.5%).
2. **The Fix (`resting_sim_indices` in `3d9cf60`):**
   - Replaced raw pointers with stable integer indices (`std::vector<size_t> resting_sim_indices`) referencing elements in `completed_orders`.
   - Once memory safety was restored, every true fill was recorded, yielding the true fill rate of **20.57%** (Dec 30) and **21.21%** (Oct 30).
3. **Sampling Strategy Differences:**
   - In Stage 3 sensitivity runs with strict 1-order-at-a-time debouncing per ticker, ~867 orders were evaluated, yielding 408 fills (47.06% on Oct 30) under positive OBI.
   - In Stage 4, continuous passive quoting (submitting an order every 250 ms across regular trading hours without restricting concurrent resting orders) generates 64,711 orders on Dec 30 and 63,218 orders on Oct 30, providing deep statistical power ($N > 60\text{k}$).
4. **Estimator-Error Re-Verification:**
   - The L2 queue estimator was re-evaluated using the memory-safe index tracker. Across both dates, depth MAE averages 49.4 sh (30.8% relative error) in shallow queues ($\le 200$ sh), dropping to 155.4 sh (2.9% relative error) in deep queues (>5k sh), confirming that trade executions dominate depth depletion at the touch.

---

## 3. Experimental Data & Headline Tables

### Regime 1: Dec 30, 2019 (Holiday Lull Low-Volatility Session)
- **Universe:** SPY, AAPL, NVDA | **Trading Hours:** 09:30:00 – 16:00:00 ET
- **Primary Baseline:** Passive Buy 100 sh @ Best Bid when OBI > 0.0, 5s TTL, 250 ms interval (64,711 submitted orders)

#### Table 1.1: Execution Comparison & Wire Latency Sweep ($\Delta t \in [0, 500\,\mu\text{s}]$)
*Confidence intervals computed via non-overlapping 5-minute time-block bootstrapping ($B = 1,000$ iterations).*
| Model | Orders | Fills | Fill Rate (95% CI) | Touch Overstate (95% CI) | Mean Lead Time (ms) |
| :--- | :---: | :---: | :---: | :---: | :---: |
| **Model 1: Naive Touch** | 64,711 | 24,266 | **37.50% [35.86% – 39.13%]** | **1.82× [1.74× – 1.92×]** | **317.5 ms [293.0 – 344.3]** |
| **Model 2: Queue L3 (0 $\mu$s)** | 64,711 | 13,314 | **20.57% [18.89% – 22.41%]** | 1.00× (Baseline) | 0.0 ms (Reference) |
| **Model 3a: Queue L3 (50 $\mu$s)** | 64,681 | 13,270 | **20.52% [18.74% – 22.42%]** | 1.00× (Baseline) | 0.0 ms (Reference) |
| **Model 3b: Queue L3 (100 $\mu$s)**| 64,662 | 13,242 | **20.48% [18.78% – 22.49%]** | 1.00× (Baseline) | 0.0 ms (Reference) |
| **Model 3c: Queue L3 (250 $\mu$s)**| 64,650 | 13,238 | **20.48% [18.80% – 22.43%]** | 1.00× (Baseline) | 0.0 ms (Reference) |
| **Model 3d: Queue L3 (500 $\mu$s)**| 64,627 | 13,203 | **20.43% [18.92% – 22.30%]** | 1.00× (Baseline) | 0.0 ms (Reference) |

#### Table 1.2: Post-Fill Mid-Price Markouts & Adverse Selection Drift (in Basis Points)
*Note: $\text{Markout}_\tau = \frac{M_\tau - P_{\text{fill}}}{P_{\text{fill}}} \times 10^4\text{ bps} = \text{HalfSpread} + \text{Drift}_\tau$*
| Model | Half-Spread (bps) | Markout 1s (bps) | Markout 5s (bps) | Markout 30s (bps) | 30s Drift (bps) |
| :--- | :---: | :---: | :---: | :---: | :---: |
| **Model 1: Naive Touch** | 0.38 | +0.28 [0.26 .. 0.31] | +0.16 [0.12 .. 0.20] | **+0.01 [-0.26 .. 0.25]** | -0.38 |
| **Model 2: Queue L3 (0 $\mu$s)** | 0.36 | +0.20 [0.18 .. 0.22] | +0.09 [0.02 .. 0.16] | **-0.05 [-0.36 .. 0.24]** | **-0.41** |
| **Model 3a: Queue L3 (50 $\mu$s)** | 0.36 | +0.20 [0.18 .. 0.23] | +0.09 [0.01 .. 0.15] | -0.04 [-0.35 .. 0.23] | -0.41 |
| **Model 3b: Queue L3 (100 $\mu$s)**| 0.36 | +0.20 [0.18 .. 0.23] | +0.09 [0.02 .. 0.16] | -0.04 [-0.32 .. 0.25] | -0.41 |
| **Model 3c: Queue L3 (250 $\mu$s)**| 0.36 | +0.20 [0.18 .. 0.23] | +0.09 [0.02 .. 0.15] | -0.04 [-0.36 .. 0.25] | -0.41 |
| **Model 3d: Queue L3 (500 $\mu$s)**| 0.36 | +0.20 [0.18 .. 0.23] | +0.10 [0.03 .. 0.16] | -0.04 [-0.37 .. 0.25] | -0.40 |

#### Table 1.3: Mark-to-Mid Markout PnL, Before Fees and Exit Costs (100 sh/fill, 95% Bootstrap CI)
*Disclosure: Marks filled inventory to prevailing mid price at horizon. Does not model liquidation or fee schedules.*
| Model | Total PnL 5s ($) | PnL/Order 5s ($) | Total PnL 30s ($) | PnL/Order 30s ($) | Total PnL 30s (95% CI) |
| :--- | :---: | :---: | :---: | :---: | :---: |
| **Model 1: Naive Touch** | **+$10,797.00** | +$0.1668 | +$722.50 | +$0.0112 | [-$17,620.50 .. +$17,509.00] |
| **Model 2: Queue L3 (0 $\mu$s)** | **+$3,382.00** | +$0.0523 | -$1,494.00 | -$0.0231 | [-$13,610.00 .. +$9,315.50] |
| **Model 3a: Queue L3 (50 $\mu$s)** | +$3,411.00 | +$0.0527 | -$1,469.50 | -$0.0227 | [-$13,467.50 .. +$8,958.00] |
| **Model 3b: Queue L3 (100 $\mu$s)**| +$3,455.00 | +$0.0534 | -$1,435.00 | -$0.0222 | [-$12,210.00 .. +$9,708.00] |
| **Model 3c: Queue L3 (250 $\mu$s)**| +$3,490.50 | +$0.0540 | -$1,447.50 | -$0.0224 | [-$13,098.50 .. +$9,293.50] |
| **Model 3d: Queue L3 (500 $\mu$s)**| +$3,603.50 | +$0.0558 | -$1,315.00 | -$0.0203 | [-$13,627.50 .. +$9,376.00] |

*Touch vs. Queue L3 5s PnL Ratio:* **3.19× [95% CI: 2.17× – 10.55×]**.

---

### Regime 2: Oct 30, 2019 (FOMC Rate Decision Volatile Session)
- **Universe:** SPY, AAPL, NVDA | **Trading Hours:** 09:30:00 – 16:00:00 ET
- **Primary Baseline:** Passive Buy 100 sh @ Best Bid when OBI > 0.0, 5s TTL, 250 ms interval (63,218 submitted orders)

#### Table 2.1: Execution Comparison & Wire Latency Sweep ($\Delta t \in [0, 500\,\mu\text{s}]$)
| Model | Orders | Fills | Fill Rate (95% CI) | Touch Overstate (95% CI) | Mean Lead Time (ms) |
| :--- | :---: | :---: | :---: | :---: | :---: |
| **Model 1: Naive Touch** | 63,218 | 23,753 | **37.57% [35.53% – 39.53%]** | **1.77× [1.71× – 1.85×]** | **368.6 ms [333.5 – 403.3]** |
| **Model 2: Queue L3 (0 $\mu$s)** | 63,218 | 13,409 | **21.21% [19.39% – 22.94%]** | 1.00× (Baseline) | 0.0 ms (Reference) |
| **Model 3a: Queue L3 (50 $\mu$s)** | 63,213 | 13,384 | **21.17% [19.39% – 23.04%]** | 1.00× (Baseline) | 0.0 ms (Reference) |
| **Model 3b: Queue L3 (100 $\mu$s)**| 63,205 | 13,348 | **21.12% [19.39% – 22.93%]** | 1.00× (Baseline) | 0.0 ms (Reference) |
| **Model 3c: Queue L3 (250 $\mu$s)**| 63,153 | 13,267 | **21.01% [19.32% – 22.85%]** | 1.00× (Baseline) | 0.0 ms (Reference) |
| **Model 3d: Queue L3 (500 $\mu$s)**| 63,059 | 13,210 | **20.95% [19.23% – 22.75%]** | 1.00× (Baseline) | 0.0 ms (Reference) |

#### Table 2.2: Post-Fill Mid-Price Markouts & Adverse Selection Drift (in Basis Points)
| Model | Half-Spread (bps) | Markout 1s (bps) | Markout 5s (bps) | Markout 30s (bps) | 30s Drift (bps) |
| :--- | :---: | :---: | :---: | :---: | :---: |
| **Model 1: Naive Touch** | 0.38 | +0.29 [0.27 .. 0.32] | +0.25 [0.20 .. 0.29] | +0.09 [-0.07 .. 0.25] | -0.29 |
| **Model 2: Queue L3 (0 $\mu$s)** | 0.35 | +0.22 [0.19 .. 0.24] | +0.20 [0.15 .. 0.26] | +0.16 [-0.00 .. 0.32] | -0.19 |
| **Model 3a: Queue L3 (50 $\mu$s)** | 0.35 | +0.22 [0.19 .. 0.25] | +0.21 [0.15 .. 0.26] | +0.16 [-0.01 .. 0.32] | -0.19 |
| **Model 3b: Queue L3 (100 $\mu$s)**| 0.35 | +0.22 [0.19 .. 0.25] | +0.21 [0.15 .. 0.26] | +0.16 [-0.01 .. 0.32] | -0.19 |
| **Model 3c: Queue L3 (250 $\mu$s)**| 0.35 | +0.22 [0.19 .. 0.25] | +0.21 [0.15 .. 0.26] | +0.16 [-0.01 .. 0.33] | -0.19 |
| **Model 3d: Queue L3 (500 $\mu$s)**| 0.35 | +0.22 [0.19 .. 0.24] | +0.20 [0.15 .. 0.25] | +0.15 [-0.02 .. 0.31] | -0.20 |

#### Table 2.3: Mark-to-Mid Markout PnL, Before Fees and Exit Costs (100 sh/fill, 95% Bootstrap CI)
| Model | Total PnL 5s ($) | PnL/Order 5s ($) | Total PnL 30s ($) | PnL/Order 30s ($) | Total PnL 30s (95% CI) |
| :--- | :---: | :---: | :---: | :---: | :---: |
| **Model 1: Naive Touch** | **+$14,542.50** | +$0.2300 | +$5,794.50 | +$0.0917 | [-$3,709.50 .. +$15,807.50] |
| **Model 2: Queue L3 (0 $\mu$s)** | **+$7,016.00** | +$0.1110 | +$5,745.00 | +$0.0909 | [+$26.00 .. +$11,707.50] |
| **Model 3a: Queue L3 (50 $\mu$s)** | +$7,032.00 | +$0.1112 | +$5,658.50 | +$0.0895 | [+$129.00 .. +$11,995.50] |
| **Model 3b: Queue L3 (100 $\mu$s)**| +$6,983.00 | +$0.1106 | +$5,478.00 | +$0.0867 | [-$247.00 .. +$11,276.00] |
| **Model 3c: Queue L3 (250 $\mu$s)**| +$6,984.00 | +$0.1106 | +$5,638.50 | +$0.0893 | [-$15.00 .. +$11,933.00] |
| **Model 3d: Queue L3 (500 $\mu$s)**| +$6,724.00 | +$0.1065 | +$5,466.50 | +$0.0866 | [-$187.00 .. +$11,238.50] |

*Touch vs. Queue L3 5s PnL Ratio:* **2.07× [95% CI: 1.81× – 2.46×]**.

---

## 4. Multiple-Testing Disclosure & Comprehensive Sensitivity Grid

### 4.1. Variant Disclosure Protocol
The primary baseline strategy parameters (100 shares, 5.0-second TTL, OBI > 0.0) were specified *a priori* as an orthodox passive quoting design before viewing Stage 4 results. To guard against selective reporting and multiple-testing bias, we evaluate the full Cartesian product across 18 parameter combinations:
- **Order Sizes:** 100 shares, 500 shares
- **Time Limits (TTL):** 1.0s, 5.0s, 30.0s
- **Imbalance Thresholds:** Unconditional (-1.0), Favorable Imbalance (OBI > 0.0), Strong Imbalance (OBI > 0.20)

### 4.2. Sensitivity Grid Results (Dec 30, 2019)
| Size (sh) | TTL | OBI Threshold | Orders | Touch Rate | True L3 Rate | Touch Overstatement | 5s Markout (bps) |
| :---: | :---: | :--- | :---: | :---: | :---: | :---: | :---: |
| 100 | 1s | Unconditional | 81,070 | 18.66% | 5.59% | **3.34×** | -0.07 |
| 100 | 1s | OBI > 0.0 | 67,946 | 17.02% | 4.92% | **3.46×** | -0.12 |
| 100 | 1s | OBI > 0.20 | 62,317 | 16.79% | 5.00% | **3.36×** | -0.11 |
| 100 | 5s | Unconditional | 74,085 | 37.98% | 22.35% | **1.70×** | +0.22 |
| 100 | 5s | OBI > 0.0 | 64,711 | 37.50% | 20.57% | **1.82×** | +0.09 |
| 100 | 5s | OBI > 0.20 | 60,331 | 36.54% | 20.08% | **1.82×** | +0.08 |
| 100 | 30s | Unconditional | 54,139 | 60.24% | 50.22% | **1.20×** | +0.72 |
| 100 | 30s | OBI > 0.0 | 49,796 | 58.99% | 47.95% | **1.23×** | +0.64 |
| 100 | 30s | OBI > 0.20 | 46,965 | 58.52% | 47.52% | **1.23×** | +0.63 |
| 500 | 1s | Unconditional | 81,070 | 18.66% | 5.59% | **3.34×** | -0.07 |
| 500 | 1s | OBI > 0.0 | 71,578 | 16.66% | 5.04% | **3.30×** | -0.09 |
| 500 | 1s | OBI > 0.20 | 67,338 | 16.08% | 4.86% | **3.31×** | -0.15 |
| 500 | 5s | Unconditional | 74,085 | 37.98% | 22.35% | **1.70×** | +0.22 |
| 500 | 5s | OBI > 0.0 | 69,174 | 37.36% | 21.36% | **1.75×** | +0.15 |
| 500 | 5s | OBI > 0.20 | 66,294 | 36.84% | 20.74% | **1.78×** | +0.16 |
| 500 | 30s | Unconditional | 54,139 | 60.24% | 50.22% | **1.20×** | +0.72 |
| 500 | 30s | OBI > 0.0 | 53,040 | 59.19% | 48.56% | **1.22×** | +0.65 |
| 500 | 30s | OBI > 0.20 | 51,849 | 58.63% | 47.99% | **1.22×** | +0.62 |

### 4.3. Sensitivity Grid Results (Oct 30, 2019)
| Size (sh) | TTL | OBI Threshold | Orders | Touch Rate | True L3 Rate | Touch Overstatement | 5s Markout (bps) |
| :---: | :---: | :--- | :---: | :---: | :---: | :---: | :---: |
| 100 | 1s | Unconditional | 78,455 | 19.24% | 6.21% | **3.10×** | +0.03 |
| 100 | 1s | OBI > 0.0 | 64,986 | 16.90% | 5.46% | **3.09×** | +0.02 |
| 100 | 1s | OBI > 0.20 | 59,226 | 16.34% | 5.42% | **3.02×** | +0.01 |
| 100 | 5s | Unconditional | 71,755 | 38.44% | 22.67% | **1.70×** | +0.27 |
| 100 | 5s | OBI > 0.0 | 63,218 | 37.57% | 21.21% | **1.77×** | +0.20 |
| 100 | 5s | OBI > 0.20 | 58,802 | 36.13% | 20.70% | **1.75×** | +0.18 |
| 100 | 30s | Unconditional | 53,533 | 58.48% | 49.77% | **1.17×** | +0.70 |
| 100 | 30s | OBI > 0.0 | 49,431 | 57.31% | 47.93% | **1.20×** | +0.60 |
| 100 | 30s | OBI > 0.20 | 46,891 | 56.49% | 47.50% | **1.19×** | +0.61 |
| 500 | 1s | Unconditional | 78,455 | 19.24% | 6.21% | **3.10×** | +0.03 |
| 500 | 1s | OBI > 0.0 | 69,072 | 17.50% | 5.72% | **3.06×** | +0.01 |
| 500 | 1s | OBI > 0.20 | 64,714 | 16.99% | 5.62% | **3.02×** | +0.03 |
| 500 | 5s | Unconditional | 71,755 | 38.44% | 22.67% | **1.70×** | +0.27 |
| 500 | 5s | OBI > 0.0 | 67,130 | 37.57% | 21.72% | **1.73×** | +0.23 |
| 500 | 5s | OBI > 0.20 | 64,585 | 36.86% | 21.42% | **1.72×** | +0.22 |
| 500 | 30s | Unconditional | 53,533 | 58.48% | 49.77% | **1.17×** | +0.70 |
| 500 | 30s | OBI > 0.0 | 52,566 | 58.70% | 49.64% | **1.18×** | +0.61 |
| 500 | 30s | OBI > 0.20 | 51,425 | 58.00% | 49.23% | **1.18×** | +0.59 |

---

## 5. Microstructure Mechanism Analysis

### 5.1. Queue Priority Mechanics
An order placed at limit price $P$ fills **if and only if incoming executions deplete all shares ahead of it in the FIFO queue plus the order's own shares**.
- The order does **not** require the entire price level to be exhausted. Any limit orders submitted after ours join the queue *behind* our order and do not impede our fill.
- The reason naive touch backtesters overstate fills is that they trigger upon *any* trade at $P$, even when the trade volume is small (e.g. a 100-share retail trade hitting a 5,000-share resting level). On these non-depleting touches:
  1. The order would not have filled in reality.
  2. The price level often holds and bounces back toward the ask.
  3. The naive backtest credits the strategy with an artificial execution that captures the half-spread under favorable price conditions.

### 5.2. Adverse Selection Drift
When incoming market sells actually do consume the thousands of shares ahead of our order, it indicates sustained, aggressive selling pressure. When our order fills, the bid level is severely depleted or broken, leading to negative post-fill price drift (-0.41 bps on Dec 30; -0.19 bps on Oct 30). This directional drift degrades the passive half-spread capture (+0.36 bps), explaining why real queue-aware executions achieve significantly lower markouts than naive backtests predict.
