#include <benchmark/benchmark.h>
#include <vector>
#include <unordered_map>
#include <string>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include "orderbook/order_book.hpp"
#include "orderbook/order_id_map.hpp"
#include "orderbook/pool.hpp"
#include "orderbook/itch_parser.hpp"
#include "../tests/reference_book.hpp"

using namespace l3;

// RAII helper for MMAP binary ITCH file in Google Benchmark
struct MmapFixture {
    int fd{-1};
    uint8_t* data{nullptr};
    size_t size{0};

    explicit MmapFixture(const std::string& path) {
        fd = open(path.c_str(), O_RDONLY);
        if (fd < 0) {
            throw std::runtime_error("Cannot open " + path);
        }
        struct stat st{};
        if (fstat(fd, &st) < 0) {
            close(fd);
            throw std::runtime_error("Cannot stat " + path);
        }
        size = static_cast<size_t>(st.st_size);
        void* addr = mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
        if (addr == MAP_FAILED) {
            close(fd);
            throw std::runtime_error("Cannot mmap " + path);
        }
        data = static_cast<uint8_t*>(addr);
        madvise(data, size, MADV_WILLNEED | MADV_SEQUENTIAL);
    }

    ~MmapFixture() {
        if (data && data != MAP_FAILED) {
            munmap(data, size);
        }
        if (fd >= 0) {
            close(fd);
        }
    }
};

// 1. Google Benchmark Handler for Arena OrderBook
struct ArenaReplayBenchmarkHandler {
    std::unordered_map<uint16_t, OrderBook> books;
    size_t count{0};

    ArenaReplayBenchmarkHandler() {
        OrderBookConfig cfg{
            .order_pool_capacity = 131072, // >4x safety margin
            .level_pool_capacity = 16384,  // >3x safety margin
            .order_map_capacity = 262144   // load factor <= 0.12
        };
        books.emplace(7451, OrderBook("SPY", 7451, cfg));
        books.emplace(13,   OrderBook("AAPL", 13, cfg));
        books.emplace(5708, OrderBook("NVDA", 5708, cfg));
    }

    void on_system_event(const SystemEventMsg&) { count++; }
    void on_stock_directory(const StockDirectoryMsg&) { count++; }

    void on_add_order(const AddOrderMsg& m) {
        count++;
        auto it = books.find(m.header.stock_locate);
        if (it != books.end()) it->second.add_order(m.order_ref, m.side, m.shares, m.price, count);
    }

    void on_order_executed(const OrderExecutedMsg& m) {
        count++;
        auto it = books.find(m.header.stock_locate);
        if (it != books.end()) it->second.execute_order(m.order_ref, m.executed_shares);
    }

    void on_order_executed_with_price(const OrderExecutedWithPriceMsg& m) {
        count++;
        auto it = books.find(m.header.stock_locate);
        if (it != books.end()) it->second.execute_order(m.order_ref, m.executed_shares);
    }

    void on_order_cancel(const OrderCancelMsg& m) {
        count++;
        auto it = books.find(m.header.stock_locate);
        if (it != books.end()) it->second.cancel_order(m.order_ref, m.canceled_shares);
    }

    void on_order_delete(const OrderDeleteMsg& m) {
        count++;
        auto it = books.find(m.header.stock_locate);
        if (it != books.end()) it->second.delete_order(m.order_ref);
    }

    void on_order_replace(const OrderReplaceMsg& m) {
        count++;
        auto it = books.find(m.header.stock_locate);
        if (it != books.end()) it->second.replace_order(m.original_order_ref, m.new_order_ref, m.shares, m.price, count);
    }

    void on_trade(const TradeMsg&) { count++; }
};

// 2. Google Benchmark Handler for Reference Book
struct ReferenceReplayBenchmarkHandler {
    std::unordered_map<uint16_t, ReferenceOrderBook> books;
    size_t count{0};

    ReferenceReplayBenchmarkHandler() {
        books.emplace(7451, ReferenceOrderBook("SPY", 7451));
        books.emplace(13,   ReferenceOrderBook("AAPL", 13));
        books.emplace(5708, ReferenceOrderBook("NVDA", 5708));
    }

    void on_system_event(const SystemEventMsg&) { count++; }
    void on_stock_directory(const StockDirectoryMsg&) { count++; }

    void on_add_order(const AddOrderMsg& m) {
        count++;
        auto it = books.find(m.header.stock_locate);
        if (it != books.end()) it->second.add_order(m.order_ref, m.side, m.shares, m.price);
    }

    void on_order_executed(const OrderExecutedMsg& m) {
        count++;
        auto it = books.find(m.header.stock_locate);
        if (it != books.end()) it->second.execute_order(m.order_ref, m.executed_shares);
    }

    void on_order_executed_with_price(const OrderExecutedWithPriceMsg& m) {
        count++;
        auto it = books.find(m.header.stock_locate);
        if (it != books.end()) it->second.execute_order(m.order_ref, m.executed_shares);
    }

    void on_order_cancel(const OrderCancelMsg& m) {
        count++;
        auto it = books.find(m.header.stock_locate);
        if (it != books.end()) it->second.cancel_order(m.order_ref, m.canceled_shares);
    }

    void on_order_delete(const OrderDeleteMsg& m) {
        count++;
        auto it = books.find(m.header.stock_locate);
        if (it != books.end()) it->second.delete_order(m.order_ref);
    }

    void on_order_replace(const OrderReplaceMsg& m) {
        count++;
        auto it = books.find(m.header.stock_locate);
        if (it != books.end()) it->second.replace_order(m.original_order_ref, m.new_order_ref, m.shares, m.price);
    }

    void on_trade(const TradeMsg&) { count++; }
};

// Replay Benchmarks (Full Stream Replay from MMAP buffer)
static void BM_ArenaBook_ReplayMmap(benchmark::State& state) {
    static MmapFixture file("data/fixture_3symbols_full.raw");
    constexpr size_t TOTAL_MSGS = 4115178;

    for (auto _ : state) {
        state.PauseTiming();
        ArenaReplayBenchmarkHandler handler;
        state.ResumeTiming();

        ItchParser::parse_buffer(file.data, file.size, handler);
        benchmark::DoNotOptimize(handler.count);
    }
    state.SetItemsProcessed(state.iterations() * TOTAL_MSGS);
}
BENCHMARK(BM_ArenaBook_ReplayMmap)->UseRealTime()->Repetitions(10)->ReportAggregatesOnly(true);

static void BM_ReferenceBook_ReplayMmap(benchmark::State& state) {
    static MmapFixture file("data/fixture_3symbols_full.raw");
    constexpr size_t TOTAL_MSGS = 4115178;

    for (auto _ : state) {
        state.PauseTiming();
        ReferenceReplayBenchmarkHandler handler;
        state.ResumeTiming();

        ItchParser::parse_buffer(file.data, file.size, handler);
        benchmark::DoNotOptimize(handler.count);
    }
    state.SetItemsProcessed(state.iterations() * TOTAL_MSGS);
}
BENCHMARK(BM_ReferenceBook_ReplayMmap)->UseRealTime()->Repetitions(10)->ReportAggregatesOnly(true);

// Existing microbenchmarks
static void BM_OrderIdMap_InsertAndFind(benchmark::State& state) {
    OrderIdMap map(65536);
    uint64_t ref = 1;

    for (auto _ : state) {
        map.insert(ref, static_cast<uint32_t>(ref & 0xFFFF));
        uint32_t val = map.find(ref);
        benchmark::DoNotOptimize(val);
        ref++;
        if (ref >= 50000) {
            state.PauseTiming();
            map.reset();
            ref = 1;
            state.ResumeTiming();
        }
    }
}
BENCHMARK(BM_OrderIdMap_InsertAndFind);

static void BM_OrderIdMap_BackwardShiftErase(benchmark::State& state) {
    OrderIdMap map(65536);
    for (uint64_t i = 1; i <= 20000; ++i) {
        map.insert(i, static_cast<uint32_t>(i));
    }

    uint64_t del_ref = 1;
    for (auto _ : state) {
        bool ok = map.erase(del_ref);
        benchmark::DoNotOptimize(ok);
        del_ref++;
        if (del_ref >= 15000) {
            state.PauseTiming();
            map.reset();
            for (uint64_t i = 1; i <= 20000; ++i) {
                map.insert(i, static_cast<uint32_t>(i));
            }
            del_ref = 1;
            state.ResumeTiming();
        }
    }
}
BENCHMARK(BM_OrderIdMap_BackwardShiftErase);

BENCHMARK_MAIN();
