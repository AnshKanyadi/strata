// TPC-H harness (P9). Loads the projected `lineitem` columns from CSV, runs the
// single-table TPC-H queries Strata supports (Q1, Q6) end-to-end via query(),
// validates each against the DuckDB oracle (double domain), and times them.
// Usage: strata_tpch [lineitem.csv]   (default /tmp/lineitem_sf1.csv)
//
// Strata has no DECIMAL type, so decimals are loaded as DOUBLE; the oracle values
// below are DuckDB's results with the same columns cast to DOUBLE (apples to
// apples). See docs/BENCHMARKS.md and ADR 0016.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include <thread>

#include "strata/exec/result_collector.hpp"
#include "strata/parallel/parallel_aggregate.hpp"
#include "strata/parallel/thread_pool.hpp"
#include "strata/plan/catalog.hpp"
#include "strata/plan/query.hpp"
#include "strata/storage/columnar_table.hpp"
#include "strata/storage/csv_loader.hpp"

namespace {

using namespace strata;

Schema LineitemSchema() {
  return Schema(std::vector<ColumnDef>{{"l_quantity", TypeId::kDouble},
                                       {"l_extendedprice", TypeId::kDouble},
                                       {"l_discount", TypeId::kDouble},
                                       {"l_tax", TypeId::kDouble},
                                       {"l_returnflag", TypeId::kVarchar},
                                       {"l_linestatus", TypeId::kVarchar},
                                       {"l_shipdate", TypeId::kDate}});
}

double Median(std::vector<double> v) {
  std::sort(v.begin(), v.end());
  return v[v.size() / 2];
}

// Run `sql`, print the result, and return the median wall-clock over `iters`.
double TimeQuery(const char* name, const char* sql, const Catalog& cat, int iters) {
  {
    auto warm = Query(sql, cat);
    if (!warm) {
      std::fprintf(stderr, "%s FAILED: %s\n", name, warm.error().message.c_str());
      return -1.0;
    }
    std::printf("\n=== %s ===\n", name);
    (*warm)->Print(std::cout);
  }
  std::vector<double> times;
  for (int i = 0; i < iters; ++i) {
    const auto t0 = std::chrono::steady_clock::now();
    auto r = Query(sql, cat);
    const auto t1 = std::chrono::steady_clock::now();
    times.push_back(std::chrono::duration<double>(t1 - t0).count());
  }
  const double ms = Median(times) * 1e3;
  std::printf("%s: %.1f ms (median of %d)\n", name, ms, iters);
  return ms;
}

bool Close(double a, double b, double rel = 1e-9) {
  return std::abs(a - b) <= rel * std::max(1.0, std::abs(b));
}

// Demonstrate the P8 morsel-parallel layer on real lineitem: a Q1-shaped
// GROUP BY l_returnflag, l_linestatus with sum(extendedprice), sum(quantity),
// count(*). This is the AGGREGATION phase only — no filter and no per-morsel
// expression args (the parallel operator aggregates table columns directly), so
// it is not the full Q1. It shows the parallel layer scaling on TPC-H data and
// connects P8 to P9. See ADR 0016.
void ParallelAggDemo(const ColumnarTable& lineitem) {
  const std::vector<GroupKey> keys{{4, TypeId::kVarchar}, {5, TypeId::kVarchar}};
  const std::vector<AggregateSpec> specs{{AggFunc::kSum, 1, TypeId::kDouble},
                                         {AggFunc::kSum, 0, TypeId::kDouble},
                                         {AggFunc::kCountStar}};
  const Schema rs(std::vector<ColumnDef>{{"rf", TypeId::kVarchar},
                                         {"ls", TypeId::kVarchar},
                                         {"se", TypeId::kDouble},
                                         {"sq", TypeId::kDouble},
                                         {"c", TypeId::kInt64}});
  std::printf("\n=== Parallel aggregation on lineitem (GROUP BY returnflag,linestatus) ===\n");
  std::printf("%-8s %-12s %-8s\n", "threads", "ms/iter", "speedup");
  double base = 0.0;
  const unsigned hw = std::max(1u, std::thread::hardware_concurrency());
  for (const std::size_t n : {std::size_t{1}, std::size_t{hw}}) {
    ThreadPool pool(n);
    {
      ResultCollector warm(rs);
      ParallelAggregate(pool, lineitem, keys, specs, warm);
    }
    std::vector<double> times;
    for (int i = 0; i < 5; ++i) {
      ResultCollector out(rs);
      const auto t0 = std::chrono::steady_clock::now();
      ParallelAggregate(pool, lineitem, keys, specs, out);
      const auto t1 = std::chrono::steady_clock::now();
      times.push_back(std::chrono::duration<double>(t1 - t0).count());
    }
    const double sec = Median(times);
    if (n == 1) base = sec;
    std::printf("%-8zu %-12.1f %-8.2f\n", n, sec * 1e3, base / sec);
  }
}

}  // namespace

int main(int argc, char** argv) {
  const char* path = (argc > 1) ? argv[1] : "/tmp/lineitem_sf1.csv";
  std::ifstream in(path);
  if (!in) {
    std::fprintf(stderr, "cannot open %s\n", path);
    return 1;
  }

  const auto l0 = std::chrono::steady_clock::now();
  auto loaded = LoadDelimited(in, LineitemSchema(), CsvOptions());
  const auto l1 = std::chrono::steady_clock::now();
  if (!loaded) {
    std::fprintf(stderr, "load failed: %s\n", loaded.error().message.c_str());
    return 1;
  }
  ColumnarTable lineitem = std::move(*loaded);
  std::printf("loaded %zu lineitem rows in %.2f s\n", lineitem.row_count(),
              std::chrono::duration<double>(l1 - l0).count());

  Catalog cat;
  cat.Add("lineitem", lineitem);

  const char* kQ6 =
      "SELECT sum(l_extendedprice * l_discount) FROM lineitem "
      "WHERE l_shipdate >= date '1994-01-01' AND l_shipdate < date '1995-01-01' "
      "AND l_discount BETWEEN 0.05 AND 0.07 AND l_quantity < 24";
  const char* kQ1 =
      "SELECT l_returnflag, l_linestatus, sum(l_quantity), sum(l_extendedprice), "
      "sum(l_extendedprice * (1 - l_discount)), "
      "sum(l_extendedprice * (1 - l_discount) * (1 + l_tax)), "
      "avg(l_quantity), avg(l_extendedprice), avg(l_discount), count(*) "
      "FROM lineitem WHERE l_shipdate <= date '1998-09-02' "
      "GROUP BY l_returnflag, l_linestatus ORDER BY l_returnflag, l_linestatus";

  TimeQuery("Q6", kQ6, cat, 7);
  TimeQuery("Q1", kQ1, cat, 7);

  // --- validate against the DuckDB oracle (SF1, double domain) ---
  bool ok = true;
  {
    auto r = Query(kQ6, cat);
    const double got = (*r)->Get<double>(0, 0);
    const bool pass = Close(got, 123141078.2282996);
    ok &= pass;
    std::printf("\nQ6 validation: got %.4f vs oracle 123141078.2283 -> %s\n", got,
                pass ? "PASS" : "MISMATCH");
  }
  {
    auto r = Query(kQ1, cat);
    const ResultCollector& q1 = **r;
    // Oracle (A,F) row: sum_qty, sum_charge; (N,O) row: count.
    struct Check { std::size_t row, col; double oracle; const char* what; };
    const Check checks[] = {
        {0, 2, 37734107.0, "AF sum_qty"},
        {0, 5, 55909065222.82747, "AF sum_charge"},
        {2, 9, 2920374.0, "NO count"},
        {3, 8, 0.05000940583017605, "RF avg_disc"},
    };
    for (const auto& c : checks) {
      const double got = (c.col == 9) ? static_cast<double>(q1.Get<std::int64_t>(c.row, c.col))
                                      : q1.Get<double>(c.row, c.col);
      const bool pass = Close(got, c.oracle);
      ok &= pass;
      std::printf("Q1 %-14s: %.6f vs %.6f -> %s\n", c.what, got, c.oracle,
                  pass ? "PASS" : "MISMATCH");
    }
  }
  std::printf("\nTPC-H validation: %s\n", ok ? "ALL PASS" : "FAILURES");

  ParallelAggDemo(lineitem);
  return ok ? 0 : 2;
}
