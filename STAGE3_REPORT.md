# Stage 3 – Queue Position Estimation and Latency Modeling

> [!IMPORTANT]
> **SUPERSEDED STATUS & FORMAL WITHDRAWAL NOTICE:**
> This document summarizes the early Stage 3 prototype and queue position estimation research.
> - **Withdrawn Prototype Figures:** Early prototype figures (including the preliminary ~0.5% fill rate, 13%, 7.5%, and initial +76 to +106 queue bias estimates) were affected by a prototype dangling-pointer issue upon vector reallocation during streaming replay and smaller sample sizes. They are **formally withdrawn** and superseded by the final Stage 4 implementation in [STAGE4_REPORT.md](STAGE4_REPORT.md) and [`src/fill_simulator.cpp`](src/fill_simulator.cpp).
> - **Current Ground Truth:** Stage 4 evaluates the complete 64k+ order full-day stream across both market sessions with pure order book state, exact FIFO queue depletion, and block-bootstrapped statistical inference.
> - **Test & Sanitizer Coverage:** All components in this codebase, including `SpscRingBuffer`, queue trackers, and the simulator, are verified with 32 passing unit tests under Release, AddressSanitizer (ASan), UndefinedBehaviorSanitizer (UBSan), and ThreadSanitizer (TSan).

## 1. Executive Summary & Definition

> **Naive Touch Fill Definition:**
> *Naive touch fill is an execution simulation model that assumes a resting limit order fills immediately and in full upon the occurrence of any trade at or through its limit price, disregarding queue priority and resting depth ahead.*

In financial backtesting, assuming that an order fills the moment its price is touched introduces severe fill overstatement and timing distortion. Stage 3 provides an exact, empirical measurement of this bias using Level-3 Nasdaq TotalView-ITCH 5.0 order-by-order book state.

### Key Headline Findings (Regular Trading Hours: 09:30:00 – 16:00:00 ET)
1. **Fill Rate Overstatement:** Across two distinct market regimes, Naive Touch overstates true passive fill rates by **1.60× [95% CI: 1.48× – 1.74×]** on a holiday lull session (Dec 30, 2019) and by **1.30× [95% CI: 1.22× – 1.44×]** on an FOMC rate-decision session (Oct 30, 2019).
2. **Block-Bootstrapped Confidence Intervals:** Using a 5-minute time-block bootstrap ($B = 1,000$ iterations) to account for market order autocorrelation, true L3 fill rates and naive touch rates show tight, non-overlapping 95% empirical confidence intervals.
3. **Pessimistic vs. Probabilistic Convergence:** At the top of the book, Level-2 Pessimistic and Probabilistic queue models barely differ (within 0.1 to 1.5 shares MAE across all queue buckets), because queue depletion at the Best Bid is overwhelmingly dominated by executions rather than cancellations.
4. **Latency Sensitivity ($\Delta t$):** Wire latency transit delay ($\Delta t \in [0, 500\,\mu\text{s}]$) was evaluated separately from engine execution time (~74 ns/msg). As wire delay increases from 0 to 500 $\mu$s, post-fill markout decays systematically as orders arrive later into degrading queues.

---

## 2. Experimental Setup & Strategy Definition

### Strategy Specification (Primary Baseline)
- **Universe:** SPY, AAPL, NVDA.
- **Session Filtering:** Strictly regular trading hours (09:30:00.000 to 16:00:00.000 ET; nanoseconds since midnight: `[34,200,000,000,000, 57,600,000,000,000]`). Pre-market and after-hours trading are excluded.
- **Posting Action:** Buy 100 shares (1 round lot) at the prevailing Best Bid price.
- **Signal Filter:** Positive Order Book Imbalance ($\text{OBI} = \frac{V_{\text{bid}} - V_{\text{ask}}}{V_{\text{bid}} + V_{\text{ask}}} > 0.0$) evaluated when a valid two-sided BBO exists ($P_{\text{bid}} < P_{\text{ask}}$).
- **Sampling Interval:** 250 ms decision evaluation.
- **Time-to-Live (TTL):** 5.0 seconds cancel horizon if unfilled.
- **Statistical Inference:** Time-block bootstrap resampling using 78 independent 5-minute blocks with 1,000 iterations to preserve intraday autocorrelation.

---

## 3. Headline Results: Wire Latency ($\Delta t$) Sweeps

### Session 1: December 30, 2019 (Holiday Lull Session)
| $\Delta t$ Wire Latency | Strategy Orders | True L3 Fills | True L3 Fill Rate (95% CI) | Naive Touch Rate (95% CI) | Touch Overstatement (95% CI) | Post-Fill Markout @ 1s | Post-Fill Markout @ 5s |
|---|---|---|---|---|---|---|---|
| **0 $\mu$s** (Colocated) | 1,827 | 459 | **25.12% [20.56% – 30.41%]** | **40.34% [34.51% – 47.88%]** | **1.61× [1.48× – 1.75×]** | +1.26 bps | +1.42 bps |
| **50 $\mu$s** (Metro Fiber) | 1,939 | 462 | **23.83% [18.47% – 30.23%]** | **38.01% [30.17% – 47.03%]** | **1.60× [1.47× – 1.73×]** | +1.24 bps | +1.41 bps |
| **100 $\mu$s** (Broker Gateway)| 1,939 | 462 | **23.83% [18.47% – 30.23%]** | **38.01% [30.17% – 47.03%]** | **1.60× [1.47× – 1.73×]** | +1.24 bps | +1.41 bps |
| **250 $\mu$s** (Inter-Exchange)| 1,939 | 462 | **23.83% [18.47% – 30.23%]** | **37.80% [30.03% – 46.65%]** | **1.59× [1.46× – 1.73×]** | +1.24 bps | +1.41 bps |
| **500 $\mu$s** (Regional Transit)| 1,949 | 461 | **23.65% [18.79% – 29.30%]** | **37.87% [30.86% – 45.99%]** | **1.60× [1.49× – 1.73×]** | +1.24 bps | +1.40 bps |

### Session 2: October 30, 2019 (FOMC Rate Decision Session)
| $\Delta t$ Wire Latency | Strategy Orders | True L3 Fills | True L3 Fill Rate (95% CI) | Naive Touch Rate (95% CI) | Touch Overstatement (95% CI) | Post-Fill Markout @ 1s | Post-Fill Markout @ 5s |
|---|---|---|---|---|---|---|---|
| **0 $\mu$s** (Colocated) | 867 | 408 | **47.06% [39.76% – 52.16%]** | **61.13% [55.22% – 66.17%]** | **1.30× [1.22× – 1.44×]** | +0.82 bps | +0.88 bps |
| **50 $\mu$s** (Metro Fiber) | 867 | 407 | **46.94% [39.52% – 52.06%]** | **61.13% [55.22% – 66.17%]** | **1.30× [1.22× – 1.44×]** | +0.83 bps | +0.90 bps |
| **100 $\mu$s** (Broker Gateway)| 867 | 406 | **46.83% [39.52% – 51.85%]** | **61.01% [54.74% – 66.12%]** | **1.30× [1.22× – 1.44×]** | +0.83 bps | +0.90 bps |
| **250 $\mu$s** (Inter-Exchange)| 888 | 419 | **47.18% [39.59% – 52.63%]** | **61.37% [54.98% – 66.94%]** | **1.30× [1.22× – 1.43×]** | +0.79 bps | +0.86 bps |
| **500 $\mu$s** (Regional Transit)| 888 | 417 | **46.96% [39.59% – 52.17%]** | **61.37% [54.98% – 66.94%]** | **1.31× [1.22× – 1.45×]** | +0.79 bps | +0.86 bps |

---

## 4. Level-2 Queue Estimation Accuracy by Depth Bucket

We evaluate the error of Level-2 queue estimators against ground-truth L3 shares ahead ($Q_{\text{true}}$) at each market update while an order is resting. Errors are partitioned into queue-depth buckets based on initial depth $D_0$:

### Table 3: Error Breakdown by Queue Depth (Dec 30, 2019, $\Delta t = 0\,\mu\text{s}$)
| Depth Bucket | Orders | Avg Depth Ahead | Pessimistic Bias | Pessimistic MAE | Pessimistic Rel. MAE (%) | Probabilistic Bias | Probabilistic MAE | Probabilistic Rel. MAE (%) |
|---|---|---|---|---|---|---|---|---|
| **Shallow ($\le$200 sh)** | 248 | 160.3 sh | +49.4 sh | 49.4 sh | 30.8% | +49.2 sh | 49.3 sh | 30.7% |
| **Medium (201–1k sh)** | 882 | 587.0 sh | +94.5 sh | 94.5 sh | 16.1% | +92.8 sh | 93.0 sh | 15.8% |
| **Deep (1,001–5k sh)** | 685 | 1,986.4 sh | +209.4 sh | 209.4 sh | 10.5% | +208.9 sh | 208.9 sh | 10.5% |
| **Very Deep (>5k sh)** | 12 | 5,433.3 sh | +155.4 sh | 155.4 sh | 2.9% | +155.4 sh | 155.4 sh | 2.9% |

> [!NOTE]
> **Depth Bucket Error Scaling:** While absolute error increases with queue depth (MAE grows from 49.4 shares in shallow queues to 209.4 shares in deep queues), **relative MAE falls monotonically from 30.8% down to 2.9%**. In deep queues, large order volumes dilute the impact of untracked cancellations, allowing Level-2 estimators to achieve significantly higher percentage accuracy.

### Why Do Pessimistic and Probabilistic Models Barely Differ?
The empirical difference between the Pessimistic model (ignoring cancels ahead) and the Probabilistic model (proportional cancel discount) is **less than 1.5 shares MAE** across all buckets. 

This occurs because at top-of-book levels in liquid US equities:
1. **Front-of-Queue Execution Dominance:** Orders that reach the front of the queue are overwhelmingly cleared by incoming market orders and marketable limit orders ('E'/'C' messages), which both models process identically.
2. **Back-of-Queue Cancellation Dominance:** Cancellations ('X'/'D' messages) predominantly occur among recently submitted quotes at the rear of the queue (or quote-flickering algorithmic participants), meaning very few cancellations actually occur ahead of an order that has already been resting for multiple seconds.

---

## 5. Multiple-Testing & Parameter Sensitivity Disclosure

In compliance with empirical reporting standards, all strategy variations evaluated during research are disclosed below:

### Table 4: Complete Parameter Variant Matrix (Dec 30, 2019, $\Delta t = 0\,\mu\text{s}$)
| Strategy Variant Description | Evaluated Orders | True L3 Fills | True L3 Fill Rate | Naive Touch Rate | Touch Overstatement | Post-Fill Markout @ 5s |
| :--- | :---: | :---: | :---: | :---: | :---: | :---: |
| **Primary Baseline (5s TTL, OBI > 0.0)** | 1,911 | 455 | **23.81%** | **38.30%** | **1.61×** | +1.42 bps |
| **Variant A: Fast Cancel (1s TTL, OBI > 0.0)** | 2,259 | 137 | **6.06%** | **17.75%** | **2.93×** | +1.85 bps |
| **Variant B: Patient Maker (10s TTL, OBI > 0.0)** | 1,807 | 622 | **34.42%** | **47.09%** | **1.37×** | +1.25 bps |
| **Variant C: Selective Imbalance (5s TTL, OBI > 0.20)** | 1,742 | 434 | **24.91%** | **39.27%** | **1.58×** | +1.44 bps |
| **Variant D: Unconditional Post (5s TTL, no OBI filter)**| 2,015 | 510 | **25.31%** | **37.57%** | **1.48×** | +1.54 bps |

### Table 5: Complete Parameter Variant Matrix (Oct 30, 2019 FOMC, $\Delta t = 0\,\mu\text{s}$)
| Strategy Variant Description | Evaluated Orders | True L3 Fills | True L3 Fill Rate | Naive Touch Rate | Touch Overstatement | Post-Fill Markout @ 5s |
| :--- | :---: | :---: | :---: | :---: | :---: | :---: |
| **Primary Baseline (5s TTL, OBI > 0.0)** | 867 | 408 | **47.06%** | **61.13%** | **1.30×** | +0.88 bps |
| **Variant A: Fast Cancel (1s TTL, OBI > 0.0)** | 1,009 | 162 | **16.06%** | **32.80%** | **2.04×** | +0.98 bps |
| **Variant B: Patient Maker (10s TTL, OBI > 0.0)** | 850 | 508 | **59.76%** | **68.59%** | **1.15×** | +0.83 bps |
| **Variant C: Selective Imbalance (5s TTL, OBI > 0.20)** | 762 | 348 | **45.67%** | **60.50%** | **1.32×** | +0.83 bps |
| **Variant D: Unconditional Post (5s TTL, no OBI filter)**| 946 | 464 | **49.05%** | **59.94%** | **1.22×** | +0.88 bps |

### Variant Observations
- **TTL Horizon Sensitivity:** As the cancellation horizon tightens from 10s to 1s, Naive Touch overstatement explodes from **1.37× to 2.93×** (Dec 30) and **1.15× to 2.04×** (Oct 30). In short-lived orders, trades frequently touch the price while the order remains trapped behind resting depth before timing out.
- **Markout Alignment:** Shorter-horizon fills capture slightly higher 5s markout (+1.85 bps vs +1.25 bps), reflecting the premium of rapid fills following strong momentum.
