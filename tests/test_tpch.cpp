#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "strata/data/data_chunk.hpp"
#include "strata/exec/result_collector.hpp"
#include "strata/plan/catalog.hpp"
#include "strata/plan/query.hpp"
#include "strata/storage/columnar_table.hpp"
#include "strata/util/date.hpp"

// End-to-end TPC-H Q1/Q6 on a small in-code `lineitem`. Expected values are
// DuckDB's results (double domain) on this exact 12-row dataset — so this is a
// committed, CI-runnable "validated against DuckDB" regression test. The SF1
// validation is the harness run recorded in docs/BENCHMARKS.md. See ADR 0016.

namespace strata {
namespace {

struct LRow {
  double q, price, disc, tax;
  const char* rf;
  const char* ls;
  int y, m, d;
};

// Same 12 rows fed to DuckDB to produce the oracle values below.
constexpr LRow kRows[] = {
    {17, 1000.00, 0.04, 0.02, "A", "F", 1996, 3, 13},
    {36, 2000.00, 0.09, 0.06, "A", "F", 1996, 4, 12},
    {8, 500.00, 0.10, 0.02, "N", "O", 1996, 1, 29},
    {28, 3000.00, 0.06, 0.06, "R", "F", 1994, 5, 15},
    {24, 2500.00, 0.05, 0.04, "N", "F", 1994, 7, 20},
    {15, 1200.00, 0.06, 0.03, "A", "F", 1994, 2, 10},
    {10, 800.00, 0.07, 0.02, "N", "O", 1994, 11, 30},
    {20, 1500.00, 0.05, 0.01, "R", "F", 1994, 9, 9},
    {19, 900.00, 0.08, 0.05, "N", "O", 1994, 6, 6},
    {11, 600.00, 0.06, 0.04, "A", "F", 1995, 3, 3},
    {30, 4000.00, 0.05, 0.07, "N", "O", 1998, 10, 15},
    {25, 3500.00, 0.07, 0.03, "R", "F", 1997, 12, 25},
};

Schema LineitemSchema() {
  return Schema(std::vector<ColumnDef>{{"l_quantity", TypeId::kDouble},
                                       {"l_extendedprice", TypeId::kDouble},
                                       {"l_discount", TypeId::kDouble},
                                       {"l_tax", TypeId::kDouble},
                                       {"l_returnflag", TypeId::kVarchar},
                                       {"l_linestatus", TypeId::kVarchar},
                                       {"l_shipdate", TypeId::kDate}});
}

ColumnarTable BuildLineitem() {
  Schema s = LineitemSchema();
  ColumnarTable t(s);
  DataChunk c;
  c.Initialize(s.types(), kVectorSize);
  std::size_t i = 0;
  for (const LRow& r : kRows) {
    c.column(0).Set<double>(i, r.q);
    c.column(1).Set<double>(i, r.price);
    c.column(2).Set<double>(i, r.disc);
    c.column(3).Set<double>(i, r.tax);
    c.column(4).Set<StringRef>(i, c.column(4).AddString(r.rf));
    c.column(5).Set<StringRef>(i, c.column(5).AddString(r.ls));
    c.column(6).Set<std::int32_t>(i, static_cast<std::int32_t>(DaysFromCivil(r.y, r.m, r.d)));
    ++i;
  }
  c.SetSize(i);
  t.AppendChunk(std::move(c));
  return t;
}

constexpr const char* kQ6 =
    "SELECT sum(l_extendedprice * l_discount) FROM lineitem "
    "WHERE l_shipdate >= date '1994-01-01' AND l_shipdate < date '1995-01-01' "
    "AND l_discount BETWEEN 0.05 AND 0.07 AND l_quantity < 24";

constexpr const char* kQ1 =
    "SELECT l_returnflag, l_linestatus, sum(l_quantity), sum(l_extendedprice), "
    "sum(l_extendedprice * (1 - l_discount)), "
    "sum(l_extendedprice * (1 - l_discount) * (1 + l_tax)), "
    "avg(l_quantity), avg(l_extendedprice), avg(l_discount), count(*) "
    "FROM lineitem WHERE l_shipdate <= date '1998-09-02' "
    "GROUP BY l_returnflag, l_linestatus ORDER BY l_returnflag, l_linestatus";

TEST(Tpch, Q6MatchesDuckDB) {
  ColumnarTable t = BuildLineitem();
  Catalog cat;
  cat.Add("lineitem", t);
  auto r = Query(kQ6, cat);
  ASSERT_TRUE(r.has_value()) << (r ? "" : r.error().message);
  ASSERT_EQ((**r).row_count(), 1u);
  EXPECT_NEAR((**r).Get<double>(0, 0), 203.0, 1e-9);  // DuckDB: 203.0
}

TEST(Tpch, Q1MatchesDuckDB) {
  ColumnarTable t = BuildLineitem();
  Catalog cat;
  cat.Add("lineitem", t);
  auto r = Query(kQ1, cat);
  ASSERT_TRUE(r.has_value()) << (r ? "" : r.error().message);
  const ResultCollector& q = **r;
  ASSERT_EQ(q.row_count(), 4u);

  // DuckDB oracle (double domain), ordered by (returnflag, linestatus):
  // cols: rf, ls, sum_qty, sum_base, sum_disc, sum_charge, avg_qty, avg_price, avg_disc, count
  struct E {
    const char* rf;
    const char* ls;
    double sum_qty, sum_base, sum_disc, sum_charge, avg_qty, avg_price, avg_disc;
    std::int64_t cnt;
  };
  const E exp[] = {
      {"A", "F", 79.0, 4800.0, 4472.0, 4656.8, 19.75, 1200.0, 0.0625, 4},
      {"N", "F", 24.0, 2500.0, 2375.0, 2470.0, 24.0, 2500.0, 0.05, 1},
      {"N", "O", 37.0, 2200.0, 2022.0, 2087.28, 12.333333333333334, 733.3333333333334,
       0.08333333333333333, 3},
      {"R", "F", 73.0, 8000.0, 7500.0, 7781.1, 24.333333333333332, 2666.6666666666665, 0.06, 3},
  };
  for (std::size_t i = 0; i < 4; ++i) {
    const E& e = exp[i];
    EXPECT_EQ(q.GetString(i, 0), e.rf) << "row " << i;
    EXPECT_EQ(q.GetString(i, 1), e.ls) << "row " << i;
    EXPECT_NEAR(q.Get<double>(i, 2), e.sum_qty, 1e-9) << "sum_qty row " << i;
    EXPECT_NEAR(q.Get<double>(i, 3), e.sum_base, 1e-6) << "sum_base row " << i;
    EXPECT_NEAR(q.Get<double>(i, 4), e.sum_disc, 1e-6) << "sum_disc row " << i;
    EXPECT_NEAR(q.Get<double>(i, 5), e.sum_charge, 1e-6) << "sum_charge row " << i;
    EXPECT_NEAR(q.Get<double>(i, 6), e.avg_qty, 1e-9) << "avg_qty row " << i;
    EXPECT_NEAR(q.Get<double>(i, 7), e.avg_price, 1e-9) << "avg_price row " << i;
    EXPECT_NEAR(q.Get<double>(i, 8), e.avg_disc, 1e-12) << "avg_disc row " << i;
    EXPECT_EQ(q.Get<std::int64_t>(i, 9), e.cnt) << "count row " << i;
  }
}

}  // namespace
}  // namespace strata
