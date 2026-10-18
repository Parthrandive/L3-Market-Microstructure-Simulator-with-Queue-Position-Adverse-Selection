# L3 Order Book & Microstructure Simulator – Build Plan

Oct 17, 2026 · @Parth

A 10-week plan (Oct 19 – Dec 27, 2026) to build a C++20 L3 order book, replay real Nasdaq ITCH data through it, and show how much a naive backtester overstates fills. It assumes about 8–10 hours a week, with college exams as the main risk.

## Roadmap

Each stage ends in a working commit, so the project is presentable even if you stop early. Stages 1–2 alone make a solid resume line; stages 3–4 make it distinctive.

| Week (starts) | Stage | Output | Gate to move on |
| --- | --- | --- | --- |
| 0 – Oct 19 | Setup | CMake project, GoogleTest, Google Benchmark, CI; read the ITCH 5.0 spec; download a sample file and cut a 3-symbol fixture (AAPL, SPY, NVDA) | Empty project builds and runs one test |
| 1–2 – Oct 26, Nov 2 | 1. ITCH parser + arena book | Zero-copy parser; 32-byte OrderNode pool; intrusive FIFO per price level; add / cancel / execute / replace | Book matches a simple `std::map` reference book on a full day of messages |
| 3 – Nov 9 | 2. Benchmarks + sanitizers | Messages/sec; p50/p99/p99.9 per message; ASan, UBSan, TSan clean | Numbers reproduced on 3 runs, CPU model recorded |
| 4–5 – Nov 16, Nov 23 | 3. Queue position + latency | SPSC ring buffer; queue-position tracker; configurable wire-to-wire delay | Estimator error measured against true L3 position |
| 6–7 – Nov 30, Dec 7 | 4. Fill simulator + adverse selection | Naive vs queue-aware vs queue-aware + latency fills; post-fill markout; OBI and VPIN | One table: PnL overstatement of the naive model |
| 8 – Dec 14 | 5. Python bridge | nanobind module; your agents place synthetic orders in the simulator | One agent run end to end through the bridge |
| 9 – Dec 21 | Write-up | README with results and limits; resume bullets from measured numbers | A stranger can build and reproduce it |

If exams eat 2–3 weeks, drop stage 5 first, then OBI/VPIN. Never drop stage 2 or the markout table.

## Definition of done

**Stage 1 – Parser and book**

- Parses a full ITCH 5.0 day without dropping or misreading a message.
- Differential test: every N messages, best bid/offer, level quantities and order counts equal a slow `std::map` reference book.
- Invariants hold throughout: no crossed book, no negative quantity, no leaked nodes in the pool.
- No heap allocation on the hot path (check with a counting allocator or `ltrace`).

* Week 0 fixture: filter the full file down to 3 symbols (AAPL, SPY, NVDA) using the stock-locate field, keeping the system-event messages. Run correctness tests on these; an all-symbol reference book takes too long.
* OrderNode is exactly 32 bytes: order ref (8), price (4), quantity (4), next and prev indices (4 + 4), price-level index (4), side and flags. Add a `static_assert` on the size. No timestamp in the node.
* Order-ID lookup is a pre-allocated open-addressed hash table (power-of-2 size, backward-shift deletion), not `std::unordered_map`. A replace message removes the old order ref and inserts the new one.
* Parse with `memcpy` into locals plus `__builtin_bswap32/64`, because ITCH is big-endian. No packed-struct casts, so UBSan stays clean.

**Stage 2 – Benchmarks**

- Report throughput (messages/sec) from Google Benchmark, and per-message latency at p50, p99 and p99.9 from recorded timestamp deltas.
- Run on a pinned core, with the CPU model and compiler flags in the README.
- ASan, UBSan and TSan are clean. On Linux, add `perf stat` counters for L1d misses and branch mispredicts.

* Per-message timings go into a pre-allocated array (or HdrHistogram), with the timer's own overhead measured and subtracted. Google Benchmark's batch averages hide the tail.
* On Apple Silicon the counter ticks at 24 MHz (about 41 ns), so time small batches and say so in the README, or run on Linux x86 with `rdtscp`.

**Stage 3 – Queue position and latency**

- Lock-free SPSC queue between the feed thread and the book thread, tested under TSan.
- Two queue models: pessimistic (cancels ahead never move you up) and probabilistic (cancels ahead are spread across the queue).
- Real ITCH gives true queue position for any order. Collapse the data to L2, estimate position with each model, and report the error against the truth. This is the distinctive result, and it matters for NSE, where you only have L2.
- Wire-to-wire delay is a simulation parameter: an order sent at t0 reaches the book at t0 + Δt. Keep it separate from measured processing latency.

**Stage 4 – Fill simulator**

- Three fill models on the same signals: naive (filled when price touches), queue-aware, queue-aware plus Δt.
- Post-fill markout of the mid price at 1s, 5s and 30s, for fills in each model. Negative markout is adverse selection.
- OBI and VPIN as features, computed in plain scalar code first.
- Final output is one table showing how much the naive model overstates PnL. That table is the headline of the project.

**Stage 5 – Python bridge**

- nanobind module exposing order submit, cancel and fill callbacks, with zero-copy arrays for features.
- One of your agents runs against the simulator and logs fills.

* Stretch, after Dec 21: a `step_l2_snapshot` adapter that feeds L2 snapshots to the queue estimator for live NSE paper trading. Check the Upstox depth and update rate first. Snapshots carry less information than an event stream, so keep this off the resume until it has run and produced results.

## Rules to keep it credible

- **Data:** validate on Nasdaq ITCH sample files (public, real L3). NSE tick-by-tick data is paid, so the NSE side uses synthetic flow or L2 snapshots, labelled as such. Check the exact download path on Nasdaq's data site before you start.
- **Numbers:** publish only what your own benchmark prints. The figures in the mock-up screenshots (140 ns, 0.8% cache miss, VPIN 0.24) are illustrations and stay out of the README and resume.
- **Two latencies:** simulated wire-to-wire delay (an input you set) and measured tick-to-trade processing time (a result you report) are different things. Name them differently everywhere.
- **Node layout:** 32-byte OrderNode with `uint32_t` indices instead of pointers. Add `alignas(64)` only if a measurement shows false sharing.
- **SIMD:** add it only if the profiler shows OBI or VPIN as a bottleneck. Otherwise leave it out.
- **perf:** Linux only, and cloud VMs often hide hardware counters. Plan a Linux machine or dual boot, or report Instruments results on Mac and say so.
- **Scope:** no new features until stage 4's table exists. The table is the project; everything else supports it.

## Resume bullet (fill after measuring)

Template, with every bracket replaced by a number your benchmark or experiment produced:

- Built a C++20 L3 limit order book and ITCH 5.0 replay engine (arena-allocated, intrusive FIFO levels, no hot-path heap allocation) processing \[X\] M messages/s with \[X\] ns p99 per update on \[CPU model\].
- Implemented queue-position estimation and a latency-aware fill simulator; showed a naive touch-fill backtest overstates PnL by \[X\]% versus queue-aware fills, with \[X\] bps adverse selection at a 5s markout.
- Exposed the simulator to Python via nanobind for use in a multi-agent trading research system.

GitHub link and the README results table go next to it.
