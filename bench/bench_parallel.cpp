// Scaling benchmark for morsel-driven parallel aggregation (ADR 0015). Times
// ParallelAggregate at 1..N threads over a large table and reports the speedup
// curve. Plain main (not Google Benchmark) so we control the internal thread
// count. Numbers + the honest P/E-core interpretation go in docs/BENCHMARKS.md.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <vector>

#include "strata/data/data_chunk.hpp"
#include "strata/exec/aggregate.hpp"
#include "strata/exec/result_collector.hpp"
#include "strata/parallel/parallel_aggregate.hpp"
#include "strata/parallel/thread_pool.hpp"
#include "strata/storage/columnar_table.hpp"

namespace {

using namespace strata;

ColumnarTable MakeTable(std::size_t n, std::int32_t num_keys) {
  Schema s(std::vector<ColumnDef>{{"k", TypeId::kInt32}, {"v", TypeId::kInt32}});
  ColumnarTable t(s);
  std::size_t done = 0;
  while (done < n) {
    const std::size_t b = std::min(kVectorSize, n - done);
    DataChunk c;
    c.Initialize(s.types(), kVectorSize);
    for (std::size_t i = 0; i < b; ++i) {
      const std::int32_t idx = static_cast<std::int32_t>(done + i);
      c.column(0).Set<std::int32_t>(i, idx % num_keys);
      c.column(1).Set<std::int32_t>(i, idx);
    }
    c.SetSize(b);
    t.AppendChunk(std::move(c));
    done += b;
  }
  return t;
}

double TimeAt(const ColumnarTable& t, std::size_t nthreads, int iters) {
  ThreadPool pool(nthreads);
  const std::vector<GroupKey> keys{{0, TypeId::kInt32}};
  const std::vector<AggregateSpec> specs{{AggFunc::kSum, 1, TypeId::kInt32}, {AggFunc::kCountStar}};
  const Schema rs(std::vector<ColumnDef>{
      {"k", TypeId::kInt32}, {"s", TypeId::kInt64}, {"c", TypeId::kInt64}});
  {
    ResultCollector warm(rs);  // warmup (page-in, thread spin-up)
    ParallelAggregate(pool, t, keys, specs, warm);
  }
  const auto start = std::chrono::steady_clock::now();
  for (int i = 0; i < iters; ++i) {
    ResultCollector out(rs);
    ParallelAggregate(pool, t, keys, specs, out);
  }
  const auto end = std::chrono::steady_clock::now();
  return std::chrono::duration<double>(end - start).count() / iters;  // seconds/iter
}

}  // namespace

int main() {
  const std::size_t kRows = 4'000'000;
  const std::int32_t kKeys = 1000;
  const int kIters = 5;
  const ColumnarTable t = MakeTable(kRows, kKeys);

  std::printf("morsel-driven parallel aggregation: %zu rows, %d groups, hw_concurrency=%u\n",
              kRows, kKeys, std::thread::hardware_concurrency());
  std::printf("%-8s %-12s %-16s %-8s\n", "threads", "ms/iter", "Mrows/s", "speedup");

  const double base = TimeAt(t, 1, kIters);
  for (const std::size_t n : {std::size_t{1}, std::size_t{2}, std::size_t{4}, std::size_t{6},
                              std::size_t{8}, std::size_t{11}}) {
    const double sec = TimeAt(t, n, kIters);
    std::printf("%-8zu %-12.2f %-16.1f %-8.2f\n", n, sec * 1e3,
                static_cast<double>(kRows) / sec / 1e6, base / sec);
  }
  return 0;
}
