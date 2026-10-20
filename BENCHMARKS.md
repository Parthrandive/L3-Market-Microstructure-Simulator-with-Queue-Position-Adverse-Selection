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
| **Decode Only** *(wire framing + byte unswapping)* | 22.16 | 22.10 | 22.76 | **185.71** | ~5.38 |
| **Decode + Arena OrderBook** | 202.05 | 200.59 | 217.10 | **20.37** | ~49.10 |
| **Decode + Reference OrderBook** (`std::map`) | 490.28 | 479.49 | 607.94 | **8.39** | ~119.14 |

### Isolated Book Mutation Speedup
Subtracting the 22.16 ms decode baseline isolates the pure order book logic:
- **Arena Book Only:** **179.89 ms** (**22.88M messages/s**, **43.7 ns/msg**)
- **Reference Book Only:** **468.12 ms** (**8.79M messages/s**, **113.8 ns/msg**)
- **Isolated Engine Speedup:** **2.60×**
- **Total In-Memory Replay Speedup:** **2.43×**
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

### Apple Silicon Timer Quantization & Batch Timing
The Apple Silicon hardware timer counter ticks at **24 MHz** (one tick every **41.67 ns**). Because an individual order book update executes in ~40–80 ns, single-message timestamping is subject to discrete timer tick quantization (e.g. 0 ns, 41.7 ns, 83.3 ns).

To eliminate clock quantization noise on macOS, throughput was sampled in batches of **64 messages**, computing $\Delta t_{\text{batch}} / 64$:

### Table 2: Batch Latency Distribution (`Batch time / 64`, 64,276 Batches, 4,113,664 Messages)
| Metric | Latency (ns / message) |
| :--- | :--- |
| **p50 (Median)** | **72.3 ns** |
| **p90** | **97.7 ns** |
| **p99** | **181.0 ns** |
| **p99.9** | **819.7 ns** |
| **Max** | 26,482.4 ns *(OS scheduler thread migration)* |

---

## 5. Tail Attribution by Structural Level Mutation

To investigate whether vector shifts (`memmove` of 8-byte `LevelRef` pairs) in `SortedLevels` contribute to tail latency spikes, every operation during full-day replay was categorized by message type and structural book state impact:

### Table 3: Latency Percentiles by Operation & Level Mutation
| Operation Category | Samples | p50 (ns) | p90 (ns) | p99 (ns) | p99.9 (ns) | Max (ns) |
| :--- | :---: | :---: | :---: | :---: | :---: | :---: |
| **Add (Existing Price Level)** | 1,626,849 | 42 | 125 | 209 | **375** | 449,667 |
| **Add (New Level Insert - `memmove`)** | 265,870 | 83 | 166 | 333 | **625** | 92,958 |
| **Execute (Level Retained)** | 78,119 | 41 | 42 | 125 | **250** | 9,292 |
| **Execute (Level Emptied - `memmove`)** | 44,241 | 41 | 42 | 84 | **250** | 132,666 |
| **Cancel (Partial Size Trim)** | 21,391 | 0 | 42 | 42 | **167** | 5,709 |
| **Delete (Level Retained)** | 1,578,354 | 41 | 42 | 125 | **250** | 203,917 |
| **Delete (Level Emptied - `memmove`)** | 224,833 | 42 | 84 | 209 | **459** | 30,958 |
| **Replace (Order Update)** | 256,528 | 166 | 333 | 458 | **1,000** | 156,916 |

### Empirical Insights
1. **New Level Insertion Cost:** Creating a new price level requires inserting into the sorted array, adding **~250 ns** at p99.9 (625 ns vs 375 ns for in-place adds). Because `LevelRef` is only 8 bytes, this cost remains under 1 microsecond even at p99.9.
2. **Level Removal Cost:** Emptied level deletion costs **~209 ns** at p99.9 (459 ns vs 250 ns for level-retained deletes).
3. **Replace Overhead:** Order replace ('U') performs an un-link, map deletion, and subsequent add, resulting in a p50 of 166 ns and p99.9 of 1,000 ns.

---

## 6. Multi-Day Empirical Sizing & Parity Verification

To prove that fixed pool capacities provide sufficient headroom across varying volatility regimes, multiple full Nasdaq ITCH trading sessions were replayed and checked against `ReferenceOrderBook`:

### Table 4: Cross-Day Peak Capacity & Sizing Headroom Table
| Session Date | Market Regime | Total Messages | Peak Resting (AAPL / NVDA / SPY) | Peak Levels (AAPL / NVDA / SPY) | Max Probes (`OrderIdMap`) | Differential Parity |
| :--- | :--- | :---: | :---: | :---: | :---: | :---: |
| **2019-12-30** | Holiday lull session | 4,115,178 | 27,110 / 12,800 / 1,949 | 5,320 / 3,126 / 756 | 10 (AAPL) | **100% PASS** (8,192 checks) |
| **2019-10-30** | FOMC Rate Cut session | 3,144,809 | 30,483 / 12,493 / 1,875 | 5,139 / 2,945 / 698 | 11 (AAPL) | **100% PASS** (6,228 checks) |

### Pool Headroom Assessment
- **OrderNode Pool:** Configured capacity = **65,536**. The largest measured peak was **30,483** (AAPL on FOMC day), utilizing **46.51%** of the pool ($>2.1\times$ safety margin).
- **PriceLevel Pool:** Configured capacity = **8,192**. The largest measured peak was **5,320** (AAPL), utilizing **64.94%** of the pool ($>1.5\times$ safety margin).
- **OrderIdMap:** Configured capacity = **131,072**. At peak load factor ~0.23, average probe length is **1.09–1.24** probes per insert, with max probe length bounded at **11**.
