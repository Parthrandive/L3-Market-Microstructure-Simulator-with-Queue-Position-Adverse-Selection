#include <benchmark/benchmark.h>
#include "orderbook/types.hpp"

static void BM_Trivial(benchmark::State& state) {
    for (auto _ : state) {
        auto side = l3::Side::Buy;
        benchmark::DoNotOptimize(side);
    }
}
BENCHMARK(BM_Trivial);

BENCHMARK_MAIN();
