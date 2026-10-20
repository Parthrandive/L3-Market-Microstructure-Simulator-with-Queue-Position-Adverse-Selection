#include <benchmark/benchmark.h>
#include <vector>
#include "orderbook/order_book.hpp"
#include "orderbook/order_id_map.hpp"
#include "orderbook/pool.hpp"
#include "../tests/reference_book.hpp"

using namespace l3;

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

static void BM_OrderBook_AddAndCancel(benchmark::State& state) {
    OrderBook book("SPY", 7451);
    uint64_t ref = 1;

    for (auto _ : state) {
        book.add_order(ref, Side::Buy, 100, 3200000);
        benchmark::DoNotOptimize(book.best_bid());
        book.cancel_order(ref, 100);
        benchmark::DoNotOptimize(book.best_bid());
        ref++;
    }
}
BENCHMARK(BM_OrderBook_AddAndCancel);

static void BM_ReferenceOrderBook_AddAndCancel(benchmark::State& state) {
    ReferenceOrderBook book("SPY", 7451);
    uint64_t ref = 1;

    for (auto _ : state) {
        book.add_order(ref, Side::Buy, 100, 3200000);
        benchmark::DoNotOptimize(book.best_bid());
        book.cancel_order(ref, 100);
        benchmark::DoNotOptimize(book.best_bid());
        ref++;
    }
}
BENCHMARK(BM_ReferenceOrderBook_AddAndCancel);

static void BM_OrderBook_TouchBBO(benchmark::State& state) {
    OrderBook book("SPY", 7451);
    // Populate book with 100 levels on each side
    for (uint32_t i = 1; i <= 100; ++i) {
        book.add_order(i, Side::Buy, 100, 3200000 - i * 100);
        book.add_order(1000 + i, Side::Sell, 100, 3200000 + i * 100);
    }

    for (auto _ : state) {
        auto bb = book.best_bid();
        auto ba = book.best_ask();
        benchmark::DoNotOptimize(bb);
        benchmark::DoNotOptimize(ba);
    }
}
BENCHMARK(BM_OrderBook_TouchBBO);

static void BM_OrderBook_ExecuteHead(benchmark::State& state) {
    OrderBook book("SPY", 7451);
    uint64_t ref = 1;

    for (auto _ : state) {
        state.PauseTiming();
        book.add_order(ref, Side::Buy, 100, 3200000);
        state.ResumeTiming();

        book.execute_order(ref, 100);
        ref++;
    }
}
BENCHMARK(BM_OrderBook_ExecuteHead);

BENCHMARK_MAIN();
