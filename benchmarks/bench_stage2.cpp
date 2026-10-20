#include <iostream>
#include <vector>
#include <string>
#include <chrono>
#include <numeric>
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include "orderbook/itch_parser.hpp"
#include "orderbook/order_book.hpp"
#include "orderbook/allocation_tracker.hpp"
#include "../tests/reference_book.hpp"

using namespace l3;

// Memory-mapped file helper
struct MmapFile {
    int fd{-1};
    uint8_t* data{nullptr};
    size_t size{0};

    explicit MmapFile(const std::string& path) {
        fd = open(path.c_str(), O_RDONLY);
        if (fd < 0) {
            std::cerr << "Failed to open " << path << std::endl;
            std::abort();
        }
        struct stat st;
        if (fstat(fd, &st) < 0) {
            close(fd);
            std::abort();
        }
        size = static_cast<size_t>(st.st_size);
        data = static_cast<uint8_t*>(mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0));
        if (data == MAP_FAILED) {
            close(fd);
            std::abort();
        }
#if defined(POSIX_MADV_WILLNEED)
        posix_madvise(data, size, POSIX_MADV_WILLNEED);
#endif
    }

    ~MmapFile() {
        if (data && data != MAP_FAILED) {
            munmap(data, size);
        }
        if (fd >= 0) {
            close(fd);
        }
    }
};

// 1. Decode Only Handler
struct DecodeOnlyHandler {
    size_t count{0};
    uint64_t sum_refs{0};

    void on_system_event(const SystemEventMsg&) { count++; }
    void on_stock_directory(const StockDirectoryMsg&) { count++; }
    void on_add_order(const AddOrderMsg& m) {
        count++;
        sum_refs += m.order_ref;
    }
    void on_order_executed(const OrderExecutedMsg& m) {
        count++;
        sum_refs += m.order_ref;
    }
    void on_order_executed_with_price(const OrderExecutedWithPriceMsg& m) {
        count++;
        sum_refs += m.order_ref;
    }
    void on_order_cancel(const OrderCancelMsg& m) {
        count++;
        sum_refs += m.order_ref;
    }
    void on_order_delete(const OrderDeleteMsg& m) {
        count++;
        sum_refs += m.order_ref;
    }
    void on_order_replace(const OrderReplaceMsg& m) {
        count++;
        sum_refs += m.new_order_ref;
    }
    void on_trade(const TradeMsg&) { count++; }
};

// 2. Decode + Arena Handler
struct ArenaMmapHandler {
    std::unordered_map<uint16_t, OrderBook> books;
    size_t count{0};

    ArenaMmapHandler() {
        OrderBookConfig cfg{
            .order_pool_capacity = 65536,
            .level_pool_capacity = 8192,
            .order_map_capacity = 131072
        };
        books.emplace(7451, OrderBook("SPY", 7451, cfg));
        books.emplace(13,   OrderBook("AAPL", 13, cfg));
        books.emplace(5708, OrderBook("NVDA", 5708, cfg));
    }

    void reset() {
        for (auto& [loc, b] : books) {
            b.clear();
        }
        count = 0;
    }

    void on_system_event(const SystemEventMsg&) { count++; }
    void on_stock_directory(const StockDirectoryMsg&) { count++; }

    void on_add_order(const AddOrderMsg& m) {
        count++;
        auto it = books.find(m.header.stock_locate);
        if (it != books.end()) {
            it->second.add_order(m.order_ref, m.side, m.shares, m.price, count);
        }
    }

    void on_order_executed(const OrderExecutedMsg& m) {
        count++;
        auto it = books.find(m.header.stock_locate);
        if (it != books.end()) {
            it->second.execute_order(m.order_ref, m.executed_shares);
        }
    }

    void on_order_executed_with_price(const OrderExecutedWithPriceMsg& m) {
        count++;
        auto it = books.find(m.header.stock_locate);
        if (it != books.end()) {
            it->second.execute_order(m.order_ref, m.executed_shares);
        }
    }

    void on_order_cancel(const OrderCancelMsg& m) {
        count++;
        auto it = books.find(m.header.stock_locate);
        if (it != books.end()) {
            it->second.cancel_order(m.order_ref, m.canceled_shares);
        }
    }

    void on_order_delete(const OrderDeleteMsg& m) {
        count++;
        auto it = books.find(m.header.stock_locate);
        if (it != books.end()) {
            it->second.delete_order(m.order_ref);
        }
    }

    void on_order_replace(const OrderReplaceMsg& m) {
        count++;
        auto it = books.find(m.header.stock_locate);
        if (it != books.end()) {
            it->second.replace_order(m.original_order_ref, m.new_order_ref, m.shares, m.price, count);
        }
    }

    void on_trade(const TradeMsg&) { count++; }
};

// 3. Decode + Reference Handler
struct ReferenceMmapHandler {
    std::unordered_map<uint16_t, ReferenceOrderBook> books;
    size_t count{0};

    ReferenceMmapHandler() {
        books.emplace(7451, ReferenceOrderBook("SPY", 7451));
        books.emplace(13,   ReferenceOrderBook("AAPL", 13));
        books.emplace(5708, ReferenceOrderBook("NVDA", 5708));
    }

    void reset() {
        books.clear();
        books.emplace(7451, ReferenceOrderBook("SPY", 7451));
        books.emplace(13,   ReferenceOrderBook("AAPL", 13));
        books.emplace(5708, ReferenceOrderBook("NVDA", 5708));
        count = 0;
    }

    void on_system_event(const SystemEventMsg&) { count++; }
    void on_stock_directory(const StockDirectoryMsg&) { count++; }

    void on_add_order(const AddOrderMsg& m) {
        count++;
        auto it = books.find(m.header.stock_locate);
        if (it != books.end()) {
            it->second.add_order(m.order_ref, m.side, m.shares, m.price);
        }
    }

    void on_order_executed(const OrderExecutedMsg& m) {
        count++;
        auto it = books.find(m.header.stock_locate);
        if (it != books.end()) {
            it->second.execute_order(m.order_ref, m.executed_shares);
        }
    }

    void on_order_executed_with_price(const OrderExecutedWithPriceMsg& m) {
        count++;
        auto it = books.find(m.header.stock_locate);
        if (it != books.end()) {
            it->second.execute_order(m.order_ref, m.executed_shares);
        }
    }

    void on_order_cancel(const OrderCancelMsg& m) {
        count++;
        auto it = books.find(m.header.stock_locate);
        if (it != books.end()) {
            it->second.cancel_order(m.order_ref, m.canceled_shares);
        }
    }

    void on_order_delete(const OrderDeleteMsg& m) {
        count++;
        auto it = books.find(m.header.stock_locate);
        if (it != books.end()) {
            it->second.delete_order(m.order_ref);
        }
    }

    void on_order_replace(const OrderReplaceMsg& m) {
        count++;
        auto it = books.find(m.header.stock_locate);
        if (it != books.end()) {
            it->second.replace_order(m.original_order_ref, m.new_order_ref, m.shares, m.price);
        }
    }

    void on_trade(const TradeMsg&) { count++; }
};

// Statistics helpers
struct BenchmarkResult {
    std::string name;
    std::vector<double> runs_ms;
    double median_ms{0};
    double min_ms{0};
    double max_ms{0};
    double throughput_mps{0};
};

BenchmarkResult analyze_runs(const std::string& name, const std::vector<double>& runs, size_t total_msgs) {
    std::vector<double> sorted = runs;
    std::sort(sorted.begin(), sorted.end());
    double med = sorted[sorted.size() / 2];
    double min_v = sorted.front();
    double max_v = sorted.back();
    double mps = (med > 0) ? (total_msgs / (med * 1e-3)) / 1e6 : 0;
    return BenchmarkResult{name, runs, med, min_v, max_v, mps};
}

// 4. Batch & Tail Attribution Handler
struct TailAttributionHandler {
    std::unordered_map<uint16_t, OrderBook> books;
    size_t count{0};

    // Category latencies in nanoseconds
    std::vector<uint32_t> lat_add_existing;
    std::vector<uint32_t> lat_add_new_level;
    std::vector<uint32_t> lat_exec_retained;
    std::vector<uint32_t> lat_exec_emptied;
    std::vector<uint32_t> lat_cancel;
    std::vector<uint32_t> lat_delete_retained;
    std::vector<uint32_t> lat_delete_emptied;
    std::vector<uint32_t> lat_replace;

    // Batch timing (batch size = 64)
    std::vector<double> batch_latencies;
    std::chrono::high_resolution_clock::time_point batch_start;
    size_t batch_counter{0};

    TailAttributionHandler() {
        OrderBookConfig cfg{
            .order_pool_capacity = 65536,
            .level_pool_capacity = 8192,
            .order_map_capacity = 131072
        };
        books.emplace(7451, OrderBook("SPY", 7451, cfg));
        books.emplace(13,   OrderBook("AAPL", 13, cfg));
        books.emplace(5708, OrderBook("NVDA", 5708, cfg));

        lat_add_existing.reserve(2000000);
        lat_add_new_level.reserve(50000);
        lat_exec_retained.reserve(150000);
        lat_exec_emptied.reserve(50000);
        lat_cancel.reserve(30000);
        lat_delete_retained.reserve(2000000);
        lat_delete_emptied.reserve(50000);
        lat_replace.reserve(300000);
        batch_latencies.reserve(70000);
    }

    void on_system_event(const SystemEventMsg&) { count++; }
    void on_stock_directory(const StockDirectoryMsg&) { count++; }

    void on_add_order(const AddOrderMsg& m) {
        count++;
        auto it = books.find(m.header.stock_locate);
        if (it != books.end()) {
            auto t0 = std::chrono::high_resolution_clock::now();
            LevelMutation mut = it->second.add_order(m.order_ref, m.side, m.shares, m.price, count);
            auto t1 = std::chrono::high_resolution_clock::now();
            auto ns = static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
            if (mut == LevelMutation::LevelCreated) {
                lat_add_new_level.push_back(ns);
            } else {
                lat_add_existing.push_back(ns);
            }
        }
        check_batch();
    }

    void on_order_executed(const OrderExecutedMsg& m) {
        count++;
        auto it = books.find(m.header.stock_locate);
        if (it != books.end()) {
            auto t0 = std::chrono::high_resolution_clock::now();
            LevelMutation mut = it->second.execute_order(m.order_ref, m.executed_shares);
            auto t1 = std::chrono::high_resolution_clock::now();
            auto ns = static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
            if (mut == LevelMutation::LevelRemoved) {
                lat_exec_emptied.push_back(ns);
            } else {
                lat_exec_retained.push_back(ns);
            }
        }
        check_batch();
    }

    void on_order_executed_with_price(const OrderExecutedWithPriceMsg& m) {
        count++;
        auto it = books.find(m.header.stock_locate);
        if (it != books.end()) {
            auto t0 = std::chrono::high_resolution_clock::now();
            LevelMutation mut = it->second.execute_order(m.order_ref, m.executed_shares);
            auto t1 = std::chrono::high_resolution_clock::now();
            auto ns = static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
            if (mut == LevelMutation::LevelRemoved) {
                lat_exec_emptied.push_back(ns);
            } else {
                lat_exec_retained.push_back(ns);
            }
        }
        check_batch();
    }

    void on_order_cancel(const OrderCancelMsg& m) {
        count++;
        auto it = books.find(m.header.stock_locate);
        if (it != books.end()) {
            auto t0 = std::chrono::high_resolution_clock::now();
            it->second.cancel_order(m.order_ref, m.canceled_shares);
            auto t1 = std::chrono::high_resolution_clock::now();
            auto ns = static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
            lat_cancel.push_back(ns);
        }
        check_batch();
    }

    void on_order_delete(const OrderDeleteMsg& m) {
        count++;
        auto it = books.find(m.header.stock_locate);
        if (it != books.end()) {
            auto t0 = std::chrono::high_resolution_clock::now();
            LevelMutation mut = it->second.delete_order(m.order_ref);
            auto t1 = std::chrono::high_resolution_clock::now();
            auto ns = static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
            if (mut == LevelMutation::LevelRemoved) {
                lat_delete_emptied.push_back(ns);
            } else {
                lat_delete_retained.push_back(ns);
            }
        }
        check_batch();
    }

    void on_order_replace(const OrderReplaceMsg& m) {
        count++;
        auto it = books.find(m.header.stock_locate);
        if (it != books.end()) {
            auto t0 = std::chrono::high_resolution_clock::now();
            it->second.replace_order(m.original_order_ref, m.new_order_ref, m.shares, m.price, count);
            auto t1 = std::chrono::high_resolution_clock::now();
            auto ns = static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
            lat_replace.push_back(ns);
        }
        check_batch();
    }

    void on_trade(const TradeMsg&) {
        count++;
        check_batch();
    }

    void start_batch() {
        batch_counter = 0;
        batch_start = std::chrono::high_resolution_clock::now();
    }

    void check_batch() {
        batch_counter++;
        if (batch_counter == 64) {
            auto now = std::chrono::high_resolution_clock::now();
            double dur_ns = static_cast<double>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(now - batch_start).count());
            batch_latencies.push_back(dur_ns / 64.0);
            batch_start = now;
            batch_counter = 0;
        }
    }
};

struct Percentiles {
    size_t count{0};
    double p50{0};
    double p90{0};
    double p99{0};
    double p999{0};
    double max_v{0};
};

template<typename T>
Percentiles compute_percentiles(std::vector<T>& vec) {
    if (vec.empty()) return Percentiles{};
    std::sort(vec.begin(), vec.end());
    size_t n = vec.size();
    return Percentiles{
        n,
        static_cast<double>(vec[n * 50 / 100]),
        static_cast<double>(vec[n * 90 / 100]),
        static_cast<double>(vec[n * 99 / 100]),
        static_cast<double>(vec[std::min(n - 1, n * 999 / 1000)]),
        static_cast<double>(vec.back())
    };
}

int main(int argc, char* argv[]) {
    std::string raw_path = (argc > 1) ? argv[1] : "data/fixture_3symbols_full.raw";
    std::cout << "Loading MMAP fixture: " << raw_path << " ...\n";
    MmapFile file(raw_path);
    std::cout << "Mapped " << file.size << " bytes (" << (file.size / (1024.0 * 1024.0)) << " MB)\n";

    constexpr size_t TOTAL_MSGS = 4115178;
    constexpr int NUM_RUNS = 10;

    std::cout << "\n========================================================================\n";
    std::cout << "STAGE 2 PART 1: MMAP REPLAY TIMINGS (10 RUNS AFTER WARMUP, NO I/O, NO ZLIB)\n";
    std::cout << "========================================================================\n";

    // Warm-up pass
    {
        std::cout << "Performing warmup run...\n";
        DecodeOnlyHandler warmup_handler;
        ItchParser::parse_buffer(file.data, file.size, warmup_handler);
    }

    // Benchmark 1: Decode Only
    std::cout << "\n[1/3] Benchmarking Decode Only (" << NUM_RUNS << " runs)...\n";
    std::vector<double> decode_runs;
    for (int i = 0; i < NUM_RUNS; ++i) {
        DecodeOnlyHandler handler;
        auto t0 = std::chrono::high_resolution_clock::now();
        ItchParser::parse_buffer(file.data, file.size, handler);
        auto t1 = std::chrono::high_resolution_clock::now();
        double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        decode_runs.push_back(ms);
        std::cout << "  Run " << (i + 1) << ": " << std::fixed << std::setprecision(2) << ms << " ms\n";
    }

    // Benchmark 2: Decode + Arena Book
    std::cout << "\n[2/3] Benchmarking Decode + Arena Order Book (" << NUM_RUNS << " runs)...\n";
    std::vector<double> arena_runs;
    ArenaMmapHandler arena_handler;
    for (int i = 0; i < NUM_RUNS; ++i) {
        arena_handler.reset();
        auto t0 = std::chrono::high_resolution_clock::now();
        ItchParser::parse_buffer(file.data, file.size, arena_handler);
        auto t1 = std::chrono::high_resolution_clock::now();
        double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        arena_runs.push_back(ms);
        std::cout << "  Run " << (i + 1) << ": " << std::fixed << std::setprecision(2) << ms << " ms\n";
    }

    // Benchmark 3: Decode + Reference Book
    std::cout << "\n[3/3] Benchmarking Decode + Reference Book (" << NUM_RUNS << " runs)...\n";
    std::vector<double> ref_runs;
    ReferenceMmapHandler ref_handler;
    for (int i = 0; i < NUM_RUNS; ++i) {
        ref_handler.reset();
        auto t0 = std::chrono::high_resolution_clock::now();
        ItchParser::parse_buffer(file.data, file.size, ref_handler);
        auto t1 = std::chrono::high_resolution_clock::now();
        double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        ref_runs.push_back(ms);
        std::cout << "  Run " << (i + 1) << ": " << std::fixed << std::setprecision(2) << ms << " ms\n";
    }

    auto res_decode = analyze_runs("Decode Only", decode_runs, TOTAL_MSGS);
    auto res_arena = analyze_runs("Decode + Arena Book", arena_runs, TOTAL_MSGS);
    auto res_ref = analyze_runs("Decode + Reference Book", ref_runs, TOTAL_MSGS);

    double arena_book_ms = res_arena.median_ms - res_decode.median_ms;
    double ref_book_ms = res_ref.median_ms - res_decode.median_ms;
    double book_speedup = ref_book_ms / arena_book_ms;
    double total_speedup = res_ref.median_ms / res_arena.median_ms;

    std::cout << "\n========================================================================\n";
    std::cout << "                  MMAP ISOLATED REPLAY SUMMARY TABLE                    \n";
    std::cout << "========================================================================\n";
    std::cout << "| Pipeline Component        | Median (ms) | Min (ms) | Max (ms) | Throughput (M msg/s) |\n";
    std::cout << "| :------------------------ | ----------: | -------: | -------: | -------------------: |\n";
    std::cout << "| " << std::left << std::setw(26) << res_decode.name << "| "
              << std::right << std::setw(11) << res_decode.median_ms << " | "
              << std::setw(8) << res_decode.min_ms << " | "
              << std::setw(8) << res_decode.max_ms << " | "
              << std::setw(20) << res_decode.throughput_mps << " |\n";
    std::cout << "| " << std::left << std::setw(26) << res_arena.name << "| "
              << std::right << std::setw(11) << res_arena.median_ms << " | "
              << std::setw(8) << res_arena.min_ms << " | "
              << std::setw(8) << res_arena.max_ms << " | "
              << std::setw(20) << res_arena.throughput_mps << " |\n";
    std::cout << "| " << std::left << std::setw(26) << res_ref.name << "| "
              << std::right << std::setw(11) << res_ref.median_ms << " | "
              << std::setw(8) << res_ref.min_ms << " | "
              << std::setw(8) << res_ref.max_ms << " | "
              << std::setw(20) << res_ref.throughput_mps << " |\n";
    std::cout << "------------------------------------------------------------------------\n";
    std::cout << "ISOLATED BOOK-ONLY PROCESSING TIME (Excluding ITCH parsing):\n";
    std::cout << "  Arena Book Only:      " << std::fixed << std::setprecision(2) << arena_book_ms << " ms ("
              << ((TOTAL_MSGS / (arena_book_ms * 1e-3)) / 1e6) << " M msg/s)\n";
    std::cout << "  Reference Book Only:  " << ref_book_ms << " ms ("
              << ((TOTAL_MSGS / (ref_book_ms * 1e-3)) / 1e6) << " M msg/s)\n";
    std::cout << "  -> Isolated Book Speedup: " << book_speedup << "x\n";
    std::cout << "  -> Total Replay Speedup:  " << total_speedup << "x\n";
    std::cout << "========================================================================\n";

    // Part 2: Zero Allocation Verification on MMAP
    std::cout << "\n========================================================================\n";
    std::cout << "STAGE 2 PART 2: ZERO-ALLOCATION PROOF ON MMAP (NO ZLIB IN THE LOOP)\n";
    std::cout << "========================================================================\n";
    arena_handler.reset();
    AllocationTracker::start();
    ItchParser::parse_buffer(file.data, file.size, arena_handler);
    AllocationTracker::stop();
    size_t allocs = AllocationTracker::count();
    std::cout << "Heap allocations during MMAP replay (operator new count): " << allocs << "\n";
    std::cout << "Zero-Allocation Invariant: " << ((allocs == 0) ? "VERIFIED (0 calls)" : "FAILED") << "\n";

    // Part 3: Batch Latency & Tail Attribution
    std::cout << "\n========================================================================\n";
    std::cout << "STAGE 2 PART 3 & 4: BATCH LATENCY DISTRIBUTION & TAIL ATTRIBUTION\n";
    std::cout << "========================================================================\n";
    TailAttributionHandler tail_handler;
    tail_handler.start_batch();
    ItchParser::parse_buffer(file.data, file.size, tail_handler);

    auto p_batch = compute_percentiles(tail_handler.batch_latencies);
    std::cout << "\nBatch Latency Distribution [Batch time / 64 (Apple Silicon 24 MHz clock, batch size = 64)]:\n";
    std::cout << "  Samples (batches): " << p_batch.count << " (" << (p_batch.count * 64) << " messages)\n";
    std::cout << "  p50:   " << std::fixed << std::setprecision(1) << p_batch.p50 << " ns/msg\n";
    std::cout << "  p90:   " << p_batch.p90 << " ns/msg\n";
    std::cout << "  p99:   " << p_batch.p99 << " ns/msg\n";
    std::cout << "  p99.9: " << p_batch.p999 << " ns/msg\n";
    std::cout << "  Max:   " << p_batch.max_v << " ns/msg\n";

    auto p_add_exist = compute_percentiles(tail_handler.lat_add_existing);
    auto p_add_new = compute_percentiles(tail_handler.lat_add_new_level);
    auto p_exec_ret = compute_percentiles(tail_handler.lat_exec_retained);
    auto p_exec_emp = compute_percentiles(tail_handler.lat_exec_emptied);
    auto p_cancel = compute_percentiles(tail_handler.lat_cancel);
    auto p_del_ret = compute_percentiles(tail_handler.lat_delete_retained);
    auto p_del_emp = compute_percentiles(tail_handler.lat_delete_emptied);
    auto p_rep = compute_percentiles(tail_handler.lat_replace);

    std::cout << "\nTAIL ATTRIBUTION TABLE BY OPERATION & STRUCTURAL MUTATION:\n";
    std::cout << "| Operation Category                  | Samples   | p50 (ns) | p90 (ns) | p99 (ns) | p99.9 (ns) | Max (ns) |\n";
    std::cout << "| :---------------------------------- | --------: | -------: | -------: | -------: | ---------: | -------: |\n";

    auto print_row = [](const std::string& name, const Percentiles& p) {
        std::cout << "| " << std::left << std::setw(35) << name << " | "
                  << std::right << std::setw(9) << p.count << " | "
                  << std::setw(8) << std::fixed << std::setprecision(0) << p.p50 << " | "
                  << std::setw(8) << p.p90 << " | "
                  << std::setw(8) << p.p99 << " | "
                  << std::setw(10) << p.p999 << " | "
                  << std::setw(8) << p.max_v << " |\n";
    };

    print_row("Add (Existing Price Level)", p_add_exist);
    print_row("Add (New Level Insert - memmove)", p_add_new);
    print_row("Execute (Level Retained)", p_exec_ret);
    print_row("Execute (Level Emptied - memmove)", p_exec_emp);
    print_row("Cancel (Partial Size Trim)", p_cancel);
    print_row("Delete (Level Retained)", p_del_ret);
    print_row("Delete (Level Emptied - memmove)", p_del_emp);
    print_row("Replace (Order Update)", p_rep);

    std::cout << "========================================================================\n";
    return 0;
}
