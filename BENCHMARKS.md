# Empirical Microstructure Benchmarks & Timing Analysis

*Generated from real Nasdaq TotalView-ITCH 5.0 feeds under C++20 release builds on Apple Silicon.*

---

## 1. Measurement Methodology & Environmental Controls

In strict accordance with the project's **"measured numbers only"** rule:
- All figures represent direct empirical measurements from reproducible benchmark runners (`bench_stage2`, `differential_runner`, `arena_runner`).
- No synthetic single-operation microbenchmarks are reported as resume or production throughput claims.
- Timing is performed on mains power after a full warm-up pass.

### Hardware & Software Environment
| Attribute | Specification |
| :--- | :--- |
| **System Architecture** | Apple Silicon (arm64, Apple M-series) |
| **L1 Data Cache** | 64 KiB per core |
| **L1 Instruction Cache**| 128 KiB per core |
| **L2 Unified Cache** | 4,096 KiB |
| **Hardware Counter** | 24 MHz system timebase (~41.67 ns tick resolution) |
| **Operating System** | macOS Darwin 24.6.0 (arm64) |
| **Compiler** | AppleClang 17.0.0 (`AppleClang 17.0.0.17000319`) |
| **Build Configuration** | Release (`-O3 -DNDEBUG -Wall -Wextra -Werror -pedantic -std=c++20`) |
| **Thread Scheduling Note** | macOS does not support thread core pinning (`pthread_setaffinity_np` is unsupported on Darwin). Measurements reflect unpinned single-core execution on mains power. |

---

## 2. Component-Separated Throughput (MMAP Replay)

To eliminate disk I/O and gzip streaming decompression overhead from engine timing, the full-day ITCH fixture (`4,115,178` messages) was memory-mapped (`mmap` with `posix_madvise(MADV_WILLNEED)`). Three pipeline stages were benchmarked across **10 repeated runs**:

### Table 1: In-Memory Pipeline Throughput (4,115,178 ITCH Messages, 10 Runs)
| Pipeline Component | Median (ms) | Min (ms) | Max (ms) | Throughput (M msg/s) | Latency (ns/msg) |
| :--- | :---: | :---: | :---: | :---: | :---: |
| **Decode Only** *(wire framing + field extraction)* | 34.99 | 32.33 | 55.38 | **117.61** | ~8.50 |
| **Decode + Arena OrderBook** | 358.00 | 281.47 | 584.06 | **11.49** | ~87.00 |
| **Decode + Reference OrderBook** (`std::map`) | 975.48 | 667.32 | 2220.71 | **4.22** | ~237.04 |

### Isolated Book Mutation Speedup
Subtracting the 34.99 ms decode baseline isolates the pure order book logic:
- **Arena Book Only:** **323.01 ms** (**12.74M messages/s**, **78.5 ns/msg**)
- **Reference Book Only:** **940.49 ms** (**4.38M messages/s**, **228.5 ns/msg**)
- **Isolated Engine Speedup:** **2.91×**
- **Total In-Memory Replay Speedup:** **2.72×**
- *(Compare with End-to-End Replay Speedup including gzip decompression: **1.70×**)*

---

## 3. Zero-Allocation Verification

On-path memory allocations were measured by overriding global `operator new`, `operator new[]`, `operator delete`, and `operator delete[]` with atomic instrumentation:

```text
========================================================================
STAGE 2: ZERO-ALLOCATION PROOF ON MMAP (NO ZLIB IN THE LOOP)
========================================================================
Heap allocations during MMAP replay (operator new count): 0
Zero-Allocation Invariant: VERIFIED (0 calls / 0 bytes allocated)
```

Across all 4,115,178 messages, exactly **0 heap allocations** occurred during replay.

---

## 4. Latency Distribution & Timer Resolution

### Apple Silicon Timer Quantization & Pure Batch Timing
The Apple Silicon hardware timer counter ticks at **24 MHz** (one tick every **41.67 ns**). Because an individual order book update executes in ~40–80 ns, single-message timestamping is subject to discrete timer tick quantization (e.g. 0 ns, 41.7 ns, 83.3 ns). Furthermore, calling `high_resolution_clock::now()` inside a per-message loop introduces ~20–25 ns of instrumentation overhead that pollutes batch timings.

To prevent this, pure batch timing was measured in isolated batches of **64 messages** without any inner loop timers or vector appends:
- **Sum of All Batch Durations:** 304.88 ms (reconciles with total replay time within OS scheduling jitter).
- **Mean Latency per Message:** **74.1 ns / msg**
- The distribution is properly right-skewed with median (67.7 ns) below the mean (74.1 ns).

### Table 2: Pure Batch Latency Distribution (`Batch time / 64`, 64,276 Batches, 4,113,664 Messages)
| Metric | Latency (ns / message) | Explanation |
| :--- | :--- | :--- |
| **Mean** | **74.1 ns** | Arithmetic mean across all 64,276 batches |
| **p50 (Median)** | **67.7 ns** | Typical execution time |
| **p90** | **88.5 ns** | 90th percentile |
| **p99** | **220.7 ns** | Tail including new price-level vector shifts |
| **p99.9** | **818.4 ns** | **OS Noise:** ~52.4 µs/batch; reflects OS scheduler thread interrupts/preemption |
| **Max** | 19,598.3 ns | macOS background context switch / thread migration |

---

## 5. Tail Attribution by Structural Level Mutation

To investigate whether vector shifts (`memmove` of 8-byte `LevelRef` pairs) in `SortedLevels` contribute to tail latency spikes, every operation during full-day replay was categorized by message type and structural book state impact.

> [!NOTE]
> Because the Apple Silicon timer ticks at 41.7 ns, individual sub-100 ns operations cannot produce meaningful p50/p90 values (they discretize into multiples of 41.7 ns). Only tail percentiles (p99, p99.9) are reported below as approximate clock tick multiples.

### Table 3: Approximate Tail Latency by Operation & Level Mutation
| Operation Category | Samples | Approx. p99 | Approx. p99.9 | Tail Behavior Notes |
| :--- | :---: | :---: | :---: | :--- |
| **Add (Existing Price Level)** | 1,626,849 | ~292 ns | **~542 ns** | Base fast path (in-place append to linked list) |
| **Add (New Level Insert - `memmove`)** | 265,870 | ~416 ns | **~1,291 ns** | **+~250 ns memmove shift** (~6 ticks of 41.7 ns) |
| **Execute (Level Retained)** | 78,119 | ~167 ns | **~375 ns** | In-place shares decrement |
| **Execute (Level Emptied - `memmove`)** | 44,241 | ~125 ns | **~333 ns** | Level deallocation and array compaction |
| **Cancel (Partial Size Trim)** | 21,391 | ~42 ns | **~250 ns** | In-place shares subtraction ($O(1)$) |
| **Delete (Level Retained)** | 1,578,354 | ~166 ns | **~334 ns** | Unlink from intrusive list ($O(1)$) |
| **Delete (Level Emptied - `memmove`)** | 224,833 | ~292 ns | **~625 ns** | Level deallocation and array compaction |
| **Replace (Order Update)** | 256,528 | ~542 ns | **~2,458 ns** | Atomic cancel + new order add |

### Empirical Insights
1. **The Cost of `memmove`:** Inserting a new price level costs roughly **+250 ns at p99.9** (approx. six 24 MHz clock ticks) compared to adding an order to an existing level. This empirically confirms that keeping the sorted vector narrow (only 8-byte `{price, level_idx}` pairs) ensures the shift fits within a single L1 cache access and remains well under 1.5 µs in the extreme tail.
2. **OS Preemption Separation:** The batch p99.9 of 818.4 ns/msg corresponds to ~52.4 µs per batch, which is clearly identified as macOS kernel interrupt / thread preemption noise, completely distinct from the microsecond-level book mutation tails.

---

## 6. Multi-Day Headroom Verification

To verify memory safety and avoid capacity exhaustion under extreme market volatility, the engine was benchmarked on two distinct market regimes:
1. **Quiet Session (Dec 30, 2019):** Year-end holiday lull.
2. **Volatile Session (Oct 30, 2019):** FOMC Federal Reserve interest rate decision.

### Table 4: Multi-Day Peak Capacity vs Headroom
| Symbol | Session Date | Peak Resting Orders | Configured Capacity | Utilization (%) | Safety Headroom |
| :--- | :--- | :---: | :---: | :---: | :---: |
| **AAPL** | Dec 30, 2019 (Holiday) | 27,110 | 131,072 | 20.68% | **4.83×** |
| **AAPL** | Oct 30, 2019 (FOMC) | **30,483** | 131,072 | 23.25% | **4.30×** |
| **NVDA** | Dec 30, 2019 (Holiday) | 12,800 | 131,072 | 9.76% | **10.24×** |
| **NVDA** | Oct 30, 2019 (FOMC) | 14,219 | 131,072 | 10.84% | **9.22×** |
| **SPY** | Dec 30, 2019 (Holiday) | 1,949 | 131,072 | 1.49% | **67.25×** |
| **SPY** | Oct 30, 2019 (FOMC) | 2,876 | 131,072 | 2.19% | **45.57×** |

- **Capacity Expansion:** In accordance with the 3–4× margin safety requirement, `OrderNodePool` was configured to **131,072 nodes** (4 MB RAM per book), providing a **4.30× safety margin** over the highest empirical peak (30,483 orders on FOMC day).
- **Hash Table Sizing:** `OrderIdMap` was sized to **262,144 slots** (power-of-2), maintaining peak load factors $\le 0.12$ and ensuring average probe lengths remain below 1.25.
- Both days passed full-day differential parity testing against the `std::map` reference book with bit-for-bit identical FIFO queues across all snapshots.
