# Stage 4: Fill Simulator, Queue-Aware Priority & Adverse Selection Analysis

**Document Status:** Publication-Grade Experimental Report  
**Author:** Parth Randive  
**Environment:** Apple Silicon (macOS, Apple M1, 24 MHz hardware timebase), Apple Clang C++20  
**Test Datasets:** Nasdaq TotalView-ITCH 5.0  
- **Dec 30, 2019:** Holiday Lull Session (Low Volatility, 4,115,178 raw messages, 67,432 regular-hours strategy orders)  
- **Oct 30, 2019:** FOMC Rate Decision Session (High Volatility, 3,115,960 raw messages, 64,102 regular-hours strategy orders)  
**Primary Baseline Strategy:** Passive Buy 100 shares @ Best Bid when Touch Imbalance $\text{OBI} > 0.0$, 5.0s TTL, sampled at 250 ms intervals strictly during Regular Trading Hours (09:30:00 to 16:00:00 ET).  
**Protocol:** Baseline chosen during Stage 3; full 18-combination grid reported.

---

## 1. Executive Summary & Definitions

Commercial backtesters and academic literature frequently simulate limit order execution using **Naive Touch Fill**:
> **Definition (Naive Touch Fill):** An execution simulation model that assumes a resting limit order fills immediately and in full upon the occurrence of any trade at or through its limit price, disregarding its FIFO queue priority, resting depth ahead, and market impact.

This report presents empirical results from **Stage 4** of our L3 market microstructure simulation engine. We directly compare three fill models on identical strategy opportunities across low-volatility and high-volatility trading days:
1. **Model 1: Naive Touch Fill** (assumes 100% execution upon first trade at or through limit price).
2. **Model 2: Queue-Aware L3 Fill** ($\Delta t = 0\,\mu\text{s}$, true intrusive FIFO queue depletion on pure order book state).
3. **Model 3: Latency-Aware L3 Fill** ($\Delta t \in \{50\,\mu\text{s}, 100\,\mu\text{s}, 250\,\mu\text{s}, 500\,\mu\text{s}\}$, wire transit delay before joining book).

### Key Headline Findings:
1. **Fill Rate Overstatement Range across TTLs:**
   - The headline **1.40× to 1.43× overstatement** on 100-share orders applies specifically to a **5-second TTL** (46.72% touch vs 33.31% full L3 on Dec 30; 44.58% touch vs 31.20% full L3 on Oct 30).
   - Across the full parameter space, touch overstatement ranges from **~1.10× at 30s TTL** up to **2.25×–2.45× at 1s TTL** for 100 shares, and reaches **7.5×–8.8× at 1s TTL** for 500-share orders.
2. **First-Fill Rate vs. Full-Fill Rate:**
   - For a 100-share order, first-fill rate and full-fill rate are close (38.19% vs 33.31% on Dec 30; 37.50% vs 31.20% on Oct 30) because market trades in round lots frequently satisfy 100 shares.
   - For a 500-share order, while the **first-fill rate remains identical** (~38.19% on Dec 30; ~37.50% on Oct 30, since the first share reaches the order at the exact same queue depth), the **full-fill rate collapses to 14.76% (Dec 30) and 12.94% (Oct 30)** because exhausting 500 shares requires substantial subsequent execution volume before TTL expiration.
3. **Premature Execution Lead Time (~485 to 491 ms):**
   - For orders that fill under both models, Naive Touch triggers **485.3 ms [95% CI: 463.9 – 508.6 ms]** earlier on Dec 30, and **490.5 ms [95% CI: 455.3 – 525.2 ms]** earlier on Oct 30. Naive backtests prematurely credit fills on benign trade touches where real orders would remain trapped deep in the queue.
4. **Adverse Selection & Spread Degradation:**
   - In Queue L3 execution, an order fills **only when executions consume the queue ahead of it**. Later arrivals join *behind* our order and do not impede our execution. Reaching our position requires significant selling into the bid level.
   - True L3 fills suffer adverse selection at the 5s horizon: post-fill mid drift is **-0.59 bps** on Dec 30 and **-0.46 bps** on Oct 30, completely erasing the passive half-spread capture (+0.10 to +0.12 bps) and producing negative net markouts.
5. **Convergence at 30s (No Significant Inversion):**
   - At the 30-second horizon, net mark-to-mid markouts for both models converge (-0.26 bps vs -0.49 bps on Dec 30; -0.17 bps vs -0.34 bps on Oct 30). With bootstrap confidence intervals spanning across broader spreads, we report **no statistically significant sign inversion**.
6. **Wire Latency Null Result:**
   - Across wire transit latencies $\Delta t \in [0, 500\,\mu\text{s}]$, fill rates decline by less than 0.1% (from 33.31% to 33.37% on Dec 30; from 31.20% to 31.13% on Oct 30), and markout PnL changes by less than 0.5%, with overlapping 95% bootstrap confidence intervals. Sub-millisecond wire latency has a statistically negligible effect compared to the first-order impact of queue priority.
7. **Replay Speedup Verification:**
   - **Google Benchmark Replay:** **2.21× speedup** (macro replay including binary ITCH decoding, 211.84 ms vs 469.01 ms median-to-median across 10 randomly interleaved repetitions).
   - **Custom Harness:** **2.60× speedup** (engine-only processing, 179.89 ms vs 468.12 ms).

---

## 2. Cross-Stage Technical Reconciliation (Stage 3 vs. Stage 4)

In an early prototype draft of Stage 3, a fill rate of ~0.5% was initially reported. In Stage 4, the measured true fill rate is ~33.3% on Dec 30 and ~31.2% on Oct 30 (for 100-share full fills). The gap between the early prototype and the final pipeline is fully resolved as follows:

1. **Root Cause: Vector Reallocation & Dangling Pointer Bug in Prototype:**
   - In the early prototype, simulated orders were stored in `std::vector<SimOrder> completed_orders`, while active orders were tracked via raw pointers `std::vector<SimOrder*> resting_sim_orders`.
   - As thousands of orders were pushed throughout the trading day, `completed_orders` reallocated, invalidating stored pointers.
   - When execution messages arrived, the engine iterated through dangling pointers to freed memory. As a result, `ord->l3_filled = true` was silently written to stale heap memory, artificially suppressing detected fills to ~0.5%.
2. **The Architectural Fix (`active_resting_indices` & Pure Book State):**
   - Replaced raw pointers with stable integer indices (`std::vector<size_t> active_resting_indices`) referencing elements in `completed_orders`.
   - Simulated orders are **never added to the underlying `OrderBook`**. `OrderBook` remains 100% pure real-market ITCH data.
   - Causal purity: Book is sampled, signal is evaluated, order arrival queue depth is recorded directly from the pure book, and order depletion is tracked externally.
   - Result: Unconditional order counts are strictly invariant to TTL (82,173 on Dec 30; 79,686 on Oct 30); OBI order counts are strictly invariant to order size (67,432 on Dec 30; 64,102 on Oct 30).
3. **Estimator-Error Re-Verification by Depth Bucket:**
   - Level-2 queue estimation errors against ground-truth L3 position show that **relative MAE falls monotonically from 30.8% in shallow queues ($\le 200$ sh) to 2.9% in deep queues (>5k sh)**, even though absolute MAE increases (from 49.4 sh to 209.4 sh). This confirms that front-of-queue depth depletion is overwhelmingly driven by executions rather than cancellations.
4. **Sanitizer Coverage:**
   - All 32 unit tests and the streaming replay pass with zero errors, zero leaks, and zero undefined behavior under AddressSanitizer (ASan), UndefinedBehaviorSanitizer (UBSan), and ThreadSanitizer (TSan).

---

## 3. Experimental Data & Headline Tables

### Regime 1: Dec 30, 2019 (Holiday Lull Low-Volatility Session)
- **Universe:** SPY, AAPL, NVDA | **Trading Hours:** 09:30:00 – 16:00:00 ET
- **Primary Baseline:** Passive Buy 100 sh @ Best Bid when OBI > 0.0, 5s TTL, 250 ms interval (67,432 submitted orders)

#### Table 1.1: Execution Comparison & Wire Latency Sweep ($\Delta t \in [0, 500\,\mu\text{s}]$)
*Confidence intervals computed via non-overlapping 5-minute time-block bootstrapping ($B = 1,000$ iterations).*
| Model | Orders | First Fills | Full Fills | First-Fill Rate (95% CI) | Full-Fill Rate (95% CI) | Touch Overstate (Full) | Mean Lead Time (ms) |
| :--- | :---: | :---: | :---: | :---: | :---: | :---: | :---: |
| **Model 1: Naive Touch** | 67,432 | 31,503 | 31,503 | **46.72% [44.28% – 49.05%]** | **46.72% [44.28% – 49.05%]** | **1.40× [1.36× – 1.45×]** | **485.3 ms [463.9 – 508.6]** |
| **Model 2: Queue L3 (0 $\mu$s)** | 67,432 | 25,754 | 22,462 | **38.19% [35.54% – 41.07%]** | **33.31% [30.86% – 36.07%]** | 1.00× (Baseline) | 0.0 ms (Reference) |
| **Model 3a: Queue L3 (50 $\mu$s)** | 67,432 | 25,852 | 22,561 | **38.34% [35.50% – 41.09%]** | **33.46% [30.72% – 36.17%]** | 1.00× (Baseline) | 0.0 ms (Reference) |
| **Model 3b: Queue L3 (100 $\mu$s)**| 67,432 | 25,845 | 22,548 | **38.33% [35.46% – 41.34%]** | **33.44% [30.70% – 36.37%]** | 1.00× (Baseline) | 0.0 ms (Reference) |
| **Model 3c: Queue L3 (250 $\mu$s)**| 67,432 | 25,818 | 22,537 | **38.29% [35.58% – 41.08%]** | **33.42% [30.82% – 36.04%]** | 1.00× (Baseline) | 0.0 ms (Reference) |
| **Model 3d: Queue L3 (500 $\mu$s)**| 67,432 | 25,786 | 22,504 | **38.24% [35.69% – 41.16%]** | **33.37% [30.91% – 36.10%]** | 1.00× (Baseline) | 0.0 ms (Reference) |

#### Table 1.2: Post-Fill Mid-Price Markouts & Adverse Selection Drift (in Basis Points)
*Note: $\text{Markout}_\tau = \frac{M_\tau - P_{\text{fill}}}{P_{\text{fill}}} \times 10^4\text{ bps} = \text{HalfSpread} + \text{Drift}_\tau$*
| Model | Half-Spread (bps) | Markout 1s (bps) | Markout 5s (bps) | Markout 30s (bps) | 30s Drift (bps) |
| :--- | :---: | :---: | :---: | :---: | :---: |
| **Model 1: Naive Touch** | +0.31 | -0.19 [-0.22 .. -0.17] | -0.27 [-0.34 .. -0.22] | -0.26 [-0.55 .. +0.02] | -0.56 |
| **Model 2: Queue L3 (0 $\mu$s)** | +0.10 | -0.41 [-0.44 .. -0.37] | -0.53 [-0.60 .. -0.45] | -0.49 [-0.82 .. -0.20] | -0.59 |
| **Model 3a: Queue L3 (50 $\mu$s)** | +0.10 | -0.41 [-0.44 .. -0.37] | -0.52 [-0.61 .. -0.46] | -0.49 [-0.84 .. -0.20] | -0.59 |
| **Model 3b: Queue L3 (100 $\mu$s)**| +0.10 | -0.41 [-0.44 .. -0.37] | -0.52 [-0.60 .. -0.45] | -0.49 [-0.80 .. -0.16] | -0.59 |
| **Model 3c: Queue L3 (250 $\mu$s)**| +0.10 | -0.41 [-0.44 .. -0.37] | -0.52 [-0.60 .. -0.45] | -0.49 [-0.80 .. -0.16] | -0.59 |
| **Model 3d: Queue L3 (500 $\mu$s)**| +0.10 | -0.41 [-0.44 .. -0.37] | -0.53 [-0.61 .. -0.46] | -0.49 [-0.85 .. -0.19] | -0.59 |

#### Table 1.3: Mark-to-Mid Markout PnL, Before Fees and Exit Costs (100 sh/fill, 95% Bootstrap CI)
*Disclosure: Marks filled inventory to prevailing mid price at horizon. Does not model liquidation or fee schedules.*
| Model | Total PnL 5s ($) | PnL/Order 5s ($) | Total PnL 30s ($) | PnL/Order 30s ($) | Total PnL 30s (95% CI) |
| :--- | :---: | :---: | :---: | :---: | :---: |
| **Model 1: Naive Touch** | -$25,273.50 | -$0.3748 | -$23,918.50 | -$0.3547 | [-$53,998.00 .. +$1,797.00] |
| **Model 2: Queue L3 (0 $\mu$s)** | -$34,265.50 | -$0.5081 | -$32,027.50 | -$0.4750 | [-$56,805.00 .. -$12,408.50] |
| **Model 3a: Queue L3 (50 $\mu$s)** | -$34,275.50 | -$0.5083 | -$32,094.00 | -$0.4759 | [-$58,242.50 .. -$12,169.00] |
| **Model 3b: Queue L3 (100 $\mu$s)**| -$34,287.00 | -$0.5085 | -$32,035.50 | -$0.4751 | [-$54,919.00 .. -$9,646.50] |
| **Model 3c: Queue L3 (250 $\mu$s)**| -$34,255.00 | -$0.5080 | -$32,082.00 | -$0.4758 | [-$55,736.00 .. -$10,036.50] |
| **Model 3d: Queue L3 (500 $\mu$s)**| -$34,299.00 | -$0.5086 | -$32,141.00 | -$0.4766 | [-$57,781.50 .. -$11,397.50] |

---

### Regime 2: Oct 30, 2019 (FOMC Rate Decision Volatile Session)
- **Universe:** SPY, AAPL, NVDA | **Trading Hours:** 09:30:00 – 16:00:00 ET
- **Primary Baseline:** Passive Buy 100 sh @ Best Bid when OBI > 0.0, 5s TTL, 250 ms interval (64,102 submitted orders)

#### Table 2.1: Execution Comparison & Wire Latency Sweep ($\Delta t \in [0, 500\,\mu\text{s}]$)
| Model | Orders | First Fills | Full Fills | First-Fill Rate (95% CI) | Full-Fill Rate (95% CI) | Touch Overstate (Full) | Mean Lead Time (ms) |
| :--- | :---: | :---: | :---: | :---: | :---: | :---: | :---: |
| **Model 1: Naive Touch** | 64,102 | 28,578 | 28,578 | **44.58% [42.28% – 46.80%]** | **44.58% [42.28% – 46.80%]** | **1.43× [1.38× – 1.48×]** | **490.5 ms [455.3 – 525.2]** |
| **Model 2: Queue L3 (0 $\mu$s)** | 64,102 | 24,036 | 20,001 | **37.50% [35.12% – 39.91%]** | **31.20% [28.85% – 33.67%]** | 1.00× (Baseline) | 0.0 ms (Reference) |
| **Model 3a: Queue L3 (50 $\mu$s)** | 64,102 | 24,056 | 20,020 | **37.53% [35.11% – 39.95%]** | **31.23% [28.77% – 33.68%]** | 1.00× (Baseline) | 0.0 ms (Reference) |
| **Model 3b: Queue L3 (100 $\mu$s)**| 64,102 | 24,030 | 20,004 | **37.49% [35.16% – 39.74%]** | **31.21% [28.90% – 33.67%]** | 1.00× (Baseline) | 0.0 ms (Reference) |
| **Model 3c: Queue L3 (250 $\mu$s)**| 64,102 | 24,010 | 19,983 | **37.46% [35.02% – 39.83%]** | **31.17% [28.81% – 33.66%]** | 1.00× (Baseline) | 0.0 ms (Reference) |
| **Model 3d: Queue L3 (500 $\mu$s)**| 64,102 | 23,972 | 19,955 | **37.40% [35.16% – 39.77%]** | **31.13% [28.92% – 33.63%]** | 1.00× (Baseline) | 0.0 ms (Reference) |

#### Table 2.2: Post-Fill Mid-Price Markouts & Adverse Selection Drift (in Basis Points)
| Model | Half-Spread (bps) | Markout 1s (bps) | Markout 5s (bps) | Markout 30s (bps) | 30s Drift (bps) |
| :--- | :---: | :---: | :---: | :---: | :---: |
| **Model 1: Naive Touch** | +0.35 | -0.13 [-0.16 .. -0.09] | -0.16 [-0.21 .. -0.11] | -0.17 [-0.38 .. +0.03] | -0.52 |
| **Model 2: Queue L3 (0 $\mu$s)** | +0.12 | -0.30 [-0.34 .. -0.26] | -0.36 [-0.43 .. -0.30] | -0.34 [-0.57 .. -0.11] | -0.46 |
| **Model 3a: Queue L3 (50 $\mu$s)** | +0.12 | -0.30 [-0.34 .. -0.26] | -0.36 [-0.43 .. -0.29] | -0.34 [-0.59 .. -0.12] | -0.46 |
| **Model 3b: Queue L3 (100 $\mu$s)**| +0.12 | -0.30 [-0.34 .. -0.26] | -0.36 [-0.43 .. -0.30] | -0.34 [-0.57 .. -0.10] | -0.46 |
| **Model 3c: Queue L3 (250 $\mu$s)**| +0.12 | -0.30 [-0.35 .. -0.26] | -0.36 [-0.43 .. -0.29] | -0.34 [-0.58 .. -0.09] | -0.46 |
| **Model 3d: Queue L3 (500 $\mu$s)**| +0.12 | -0.30 [-0.35 .. -0.26] | -0.36 [-0.43 .. -0.30] | -0.34 [-0.58 .. -0.11] | -0.46 |

#### Table 2.3: Mark-to-Mid Markout PnL, Before Fees and Exit Costs (100 sh/fill, 95% Bootstrap CI)
| Model | Total PnL 5s ($) | PnL/Order 5s ($) | Total PnL 30s ($) | PnL/Order 30s ($) | Total PnL 30s (95% CI) |
| :--- | :---: | :---: | :---: | :---: | :---: |
| **Model 1: Naive Touch** | -$11,597.50 | -$0.1809 | -$11,077.50 | -$0.1728 | [-$26,858.00 .. +$3,065.50] |
| **Model 2: Queue L3 (0 $\mu$s)** | -$18,488.00 | -$0.2884 | -$16,008.50 | -$0.2497 | [-$29,619.50 .. -$3,671.00] |
| **Model 3a: Queue L3 (50 $\mu$s)** | -$18,439.50 | -$0.2877 | -$15,968.50 | -$0.2491 | [-$29,956.00 .. -$4,308.00] |
| **Model 3b: Queue L3 (100 $\mu$s)**| -$18,430.50 | -$0.2875 | -$16,054.50 | -$0.2505 | [-$29,133.00 .. -$3,224.50] |
| **Model 3c: Queue L3 (250 $\mu$s)**| -$18,461.50 | -$0.2880 | -$16,027.50 | -$0.2500 | [-$29,368.00 .. -$2,844.00] |
| **Model 3d: Queue L3 (500 $\mu$s)**| -$18,484.00 | -$0.2884 | -$16,044.00 | -$0.2503 | [-$29,943.50 .. -$3,297.00] |

---

## 4. Multiple-Testing Disclosure & Comprehensive Sensitivity Grid

### 4.1. Variant Disclosure Protocol
The primary baseline strategy parameters (100 shares, 5.0-second TTL, OBI > 0.0) were selected during Stage 3 as an orthodox passive quoting benchmark. To guard against selective reporting and multiple-testing bias, we report the complete Cartesian product across all 18 parameter combinations:
- **Order Sizes:** 100 shares, 500 shares
- **Time Limits (TTL):** 1.0s, 5.0s, 30.0s
- **Imbalance Thresholds:** Unconditional (-1.0), Favorable Imbalance (OBI > 0.0), Strong Imbalance (OBI > 0.20)

### 4.2. Sensitivity Grid Results (Dec 30, 2019)
| Size (sh) | TTL | OBI Threshold | Orders | Touch Rate | L3 First-Fill | L3 Full-Fill | Touch Overstate (Full) | 5s Markout (bps) |
| :---: | :---: | :--- | :---: | :---: | :---: | :---: | :---: | :---: |
| 100 | 1s | Unconditional | 82,173 | 21.45% | 11.45% | 8.87% | **2.42×** | -0.50 |
| 100 | 1s | OBI > 0.0 | 67,432 | 18.54% | 10.29% | 8.10% | **2.29×** | -0.48 |
| 100 | 1s | OBI > 0.20 | 61,259 | 18.38% | 10.44% | 8.29% | **2.22×** | -0.49 |
| 100 | 5s | Unconditional | 82,173 | 49.90% | 40.53% | 35.14% | **1.42×** | -0.48 |
| 100 | 5s | OBI > 0.0 | 67,432 | 46.72% | 38.19% | 33.31% | **1.40×** | -0.53 |
| 100 | 5s | OBI > 0.20 | 61,259 | 46.29% | 38.06% | 33.27% | **1.39×** | -0.54 |
| 100 | 30s | Unconditional | 82,173 | 77.91% | 74.59% | 70.94% | **1.10×** | -0.43 |
| 100 | 30s | OBI > 0.0 | 67,432 | 76.11% | 72.87% | 69.45% | **1.10×** | -0.45 |
| 100 | 30s | OBI > 0.20 | 61,259 | 76.00% | 72.83% | 69.44% | **1.09×** | -0.46 |
| 500 | 1s | Unconditional | 82,173 | 21.45% | 11.45% | 2.44% | **8.80×** | -0.72 |
| 500 | 1s | OBI > 0.0 | 67,432 | 18.54% | 10.29% | 2.41% | **7.69×** | -0.73 |
| 500 | 1s | OBI > 0.20 | 61,259 | 18.38% | 10.44% | 2.48% | **7.41×** | -0.75 |
| 500 | 5s | Unconditional | 82,173 | 49.90% | 40.53% | 15.13% | **3.30×** | -0.91 |
| 500 | 5s | OBI > 0.0 | 67,432 | 46.72% | 38.19% | 14.76% | **3.16×** | -0.96 |
| 500 | 5s | OBI > 0.20 | 61,259 | 46.29% | 38.06% | 14.77% | **3.14×** | -0.99 |
| 500 | 30s | Unconditional | 82,173 | 77.91% | 74.59% | 54.14% | **1.44×** | -0.63 |
| 500 | 30s | OBI > 0.0 | 67,432 | 76.11% | 72.87% | 52.89% | **1.44×** | -0.66 |
| 500 | 30s | OBI > 0.20 | 61,259 | 76.00% | 72.83% | 52.74% | **1.44×** | -0.67 |

### 4.3. Sensitivity Grid Results (Oct 30, 2019)
| Size (sh) | TTL | OBI Threshold | Orders | Touch Rate | L3 First-Fill | L3 Full-Fill | Touch Overstate (Full) | 5s Markout (bps) |
| :---: | :---: | :--- | :---: | :---: | :---: | :---: | :---: | :---: |
| 100 | 1s | Unconditional | 79,686 | 21.52% | 12.06% | 8.78% | **2.45×** | -0.31 |
| 100 | 1s | OBI > 0.0 | 64,102 | 17.82% | 10.50% | 7.91% | **2.25×** | -0.28 |
| 100 | 1s | OBI > 0.20 | 58,084 | 17.46% | 10.42% | 7.85% | **2.23×** | -0.29 |
| 100 | 5s | Unconditional | 79,686 | 49.36% | 41.31% | 34.02% | **1.45×** | -0.37 |
| 100 | 5s | OBI > 0.0 | 64,102 | 44.58% | 37.50% | 31.20% | **1.43×** | -0.36 |
| 100 | 5s | OBI > 0.20 | 58,084 | 43.72% | 36.69% | 30.48% | **1.43×** | -0.37 |
| 100 | 30s | Unconditional | 79,686 | 76.72% | 73.88% | 69.82% | **1.10×** | -0.35 |
| 100 | 30s | OBI > 0.0 | 64,102 | 73.26% | 70.55% | 66.60% | **1.10×** | -0.35 |
| 100 | 30s | OBI > 0.20 | 58,084 | 72.43% | 69.64% | 65.72% | **1.10×** | -0.37 |
| 500 | 1s | Unconditional | 79,686 | 21.52% | 12.06% | 2.47% | **8.71×** | -0.51 |
| 500 | 1s | OBI > 0.0 | 64,102 | 17.82% | 10.50% | 2.39% | **7.46×** | -0.45 |
| 500 | 1s | OBI > 0.20 | 58,084 | 17.46% | 10.42% | 2.39% | **7.29×** | -0.41 |
| 500 | 5s | Unconditional | 79,686 | 49.36% | 41.31% | 13.87% | **3.56×** | -0.71 |
| 500 | 5s | OBI > 0.0 | 64,102 | 44.58% | 37.50% | 12.94% | **3.45×** | -0.72 |
| 500 | 5s | OBI > 0.20 | 58,084 | 43.72% | 36.69% | 12.52% | **3.49×** | -0.72 |
| 500 | 30s | Unconditional | 79,686 | 76.72% | 73.88% | 51.09% | **1.50×** | -0.52 |
| 500 | 30s | OBI > 0.0 | 64,102 | 73.26% | 70.55% | 48.48% | **1.51×** | -0.53 |
| 500 | 30s | OBI > 0.20 | 58,084 | 72.43% | 69.64% | 47.34% | **1.53×** | -0.55 |

---

## 5. Microstructure Mechanism Analysis

### 5.1. Queue Priority & Fill Size Mechanics
An order placed at limit price $P$ fills **if and only if incoming executions deplete all shares ahead of it in the FIFO queue plus the order's own shares**.
- **First-Fill vs Full-Fill Dynamics:** Both 100-share and 500-share orders arrive at the tail of the existing queue, meaning both require the exact same depth ahead to be exhausted before receiving their first execution share. As shown in the grid, First-Fill Rates are identical (e.g. 40.53% for both 100 and 500 sh at 5s Unconditional).
- **Execution Exhaustion:** To reach a Full Fill, a 500-share order requires an additional 400 shares of aggressive selling at that exact price before TTL expiration. In fast-moving markets, trading frequently bounces after clearing 100–200 shares, leaving 500-share orders partially filled and exposed to inventory risk. Consequently, Full-Fill Rate drops to 14.76% (Dec 30) and 12.94% (Oct 30).
- **Naive Backtest Fallacy:** Naive touch backtesters trigger upon *any* trade at $P$, even when the trade volume is a single 100-share retail trade hitting a 5,000-share resting level. On these non-depleting touches:
  1. The order would not have filled in reality.
  2. The price level often holds and bounces back toward the ask.
  3. The naive backtest credits the strategy with an artificial execution that captures the half-spread under favorable price conditions.

### 5.2. Adverse Selection Drift
When incoming market sells actually do consume the thousands of shares ahead of our order, it indicates sustained, aggressive selling pressure. When our order fills, the bid level is severely depleted or broken, leading to negative post-fill price drift (-0.59 bps on Dec 30; -0.46 bps on Oct 30). This directional drift completely overwhelms the passive half-spread capture (+0.10 to +0.12 bps), explaining why real queue-aware executions achieve significantly lower markouts than naive backtests predict.
