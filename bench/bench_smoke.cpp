// A trivial benchmark whose only job in P0 is to prove the Google Benchmark
// harness is wired and runnable in CI. The real microbenchmarks (scalar vs
// SIMD kernels) arrive in P3. We use kVectorSize so the smoke test already
// operates on a real batch's worth of data.

#include <benchmark/benchmark.h>

#include <cstdint>
#include <numeric>
#include <vector>

#include "strata/config.hpp"

namespace {

// Sum a batch of int32 -> int64 (the SUM accumulator-widening policy we'll use
// for real in P4). Here it just exercises the harness.
void BM_SumBatch(benchmark::State& state) {
  std::vector<std::int32_t> data(strata::kVectorSize);
  std::iota(data.begin(), data.end(), 1);
  for (auto _ : state) {
    std::int64_t sum = 0;
    for (const std::int32_t v : data) sum += v;
    benchmark::DoNotOptimize(sum);
  }
  state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations()) *
                          static_cast<std::int64_t>(data.size()));
}
BENCHMARK(BM_SumBatch);

} // namespace

BENCHMARK_MAIN();
