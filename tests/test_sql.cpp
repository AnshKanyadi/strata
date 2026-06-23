#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "strata/exec/result_collector.hpp"
#include "strata/plan/catalog.hpp"
#include "strata/plan/query.hpp"
#include "strata/storage/columnar_table.hpp"
#include "strata/storage/csv_loader.hpp"

namespace strata {
namespace {

// table t(id, k, v):
//   (1,10,100) (2,20,5) (3,10,50) (4,20,40) (5,10,7)
ColumnarTable LoadT() {
  Schema s(std::vector<ColumnDef>{
      {"id", TypeId::kInt32}, {"k", TypeId::kInt32}, {"v", TypeId::kInt32}});
  std::istringstream in("1,10,100\n2,20,5\n3,10,50\n4,20,40\n5,10,7\n");
  auto r = LoadDelimited(in, std::move(s), CsvOptions());
  EXPECT_TRUE(r.has_value());
  return std::move(*r);
}

TEST(Sql, FilterAndOrder) {
  ColumnarTable t = LoadT();
  Catalog cat;
  cat.Add("t", t);
  auto r = Query("SELECT id, v FROM t WHERE v > 10 ORDER BY id", cat);
  ASSERT_TRUE(r.has_value()) << (r ? "" : r.error().message);
  const ResultCollector& out = **r;
  ASSERT_EQ(out.row_count(), 3u);
  EXPECT_EQ(out.Get<std::int32_t>(0, 0), 1);
  EXPECT_EQ(out.Get<std::int32_t>(0, 1), 100);
  EXPECT_EQ(out.Get<std::int32_t>(1, 0), 3);
  EXPECT_EQ(out.Get<std::int32_t>(2, 0), 4);
}

TEST(Sql, GroupByAggregate) {
  ColumnarTable t = LoadT();
  Catalog cat;
  cat.Add("t", t);
  auto r = Query("SELECT k, sum(v), count(*) FROM t GROUP BY k ORDER BY k", cat);
  ASSERT_TRUE(r.has_value()) << (r ? "" : r.error().message);
  const ResultCollector& out = **r;
  ASSERT_EQ(out.row_count(), 2u);
  EXPECT_EQ(out.Get<std::int32_t>(0, 0), 10);
  EXPECT_EQ(out.Get<std::int64_t>(0, 1), 157);  // 100+50+7
  EXPECT_EQ(out.Get<std::int64_t>(0, 2), 3);
  EXPECT_EQ(out.Get<std::int32_t>(1, 0), 20);
  EXPECT_EQ(out.Get<std::int64_t>(1, 1), 45);  // 5+40
  EXPECT_EQ(out.Get<std::int64_t>(1, 2), 2);
}

TEST(Sql, GlobalCountWithFilter) {
  ColumnarTable t = LoadT();
  Catalog cat;
  cat.Add("t", t);
  auto r = Query("SELECT count(*) FROM t WHERE v > 10", cat);
  ASSERT_TRUE(r.has_value()) << (r ? "" : r.error().message);
  ASSERT_EQ((**r).row_count(), 1u);
  EXPECT_EQ((**r).Get<std::int64_t>(0, 0), 3);
}

TEST(Sql, OrderByLimitIsTopN) {
  ColumnarTable t = LoadT();
  Catalog cat;
  cat.Add("t", t);
  auto r = Query("SELECT id, v FROM t ORDER BY v DESC LIMIT 2", cat);
  ASSERT_TRUE(r.has_value()) << (r ? "" : r.error().message);
  const ResultCollector& out = **r;
  ASSERT_EQ(out.row_count(), 2u);
  EXPECT_EQ(out.Get<std::int32_t>(0, 0), 1);   // v=100
  EXPECT_EQ(out.Get<std::int32_t>(0, 1), 100);
  EXPECT_EQ(out.Get<std::int32_t>(1, 0), 3);   // v=50
}

TEST(Sql, ProjectionArithmetic) {
  ColumnarTable t = LoadT();
  Catalog cat;
  cat.Add("t", t);
  auto r = Query("SELECT v * 2 FROM t WHERE id = 1", cat);
  ASSERT_TRUE(r.has_value()) << (r ? "" : r.error().message);
  ASSERT_EQ((**r).row_count(), 1u);
  EXPECT_EQ((**r).Get<std::int32_t>(0, 0), 200);
}

TEST(Sql, GlobalMinMaxAvg) {
  ColumnarTable t = LoadT();
  Catalog cat;
  cat.Add("t", t);
  auto r = Query("SELECT min(v), max(v), avg(v) FROM t", cat);
  ASSERT_TRUE(r.has_value()) << (r ? "" : r.error().message);
  const ResultCollector& out = **r;
  ASSERT_EQ(out.row_count(), 1u);
  EXPECT_EQ(out.Get<std::int32_t>(0, 0), 5);
  EXPECT_EQ(out.Get<std::int32_t>(0, 1), 100);
  EXPECT_DOUBLE_EQ(out.Get<double>(0, 2), 202.0 / 5.0);  // 40.4
}

TEST(Sql, UnknownColumnIsError) {
  ColumnarTable t = LoadT();
  Catalog cat;
  cat.Add("t", t);
  EXPECT_FALSE(Query("SELECT nope FROM t", cat).has_value());
  EXPECT_FALSE(Query("SELECT id FROM missing_table", cat).has_value());
}

}  // namespace
}  // namespace strata
