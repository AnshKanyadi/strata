#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "strata/data/data_chunk.hpp"
#include "strata/exec/aggregate.hpp"
#include "strata/exec/hash_aggregate.hpp"
#include "strata/exec/result_collector.hpp"
#include "strata/storage/columnar_table.hpp"

namespace strata {
namespace {

Schema SchemaOf(std::initializer_list<TypeId> types) {
  std::vector<ColumnDef> cols;
  int i = 0;
  for (const TypeId t : types) cols.push_back({"c" + std::to_string(i++), t});
  return Schema(std::move(cols));
}

// Find the result row whose int32 key column equals `key` (groups come out in
// first-seen order, so tests look up by key rather than assume an order).
std::optional<std::size_t> FindI32(const ResultCollector& c, std::size_t key_col, std::int32_t key) {
  for (std::size_t r = 0; r < c.row_count(); ++r) {
    if (!c.IsNull(r, key_col) && c.Get<std::int32_t>(r, key_col) == key) return r;
  }
  return std::nullopt;
}

TEST(HashAggregate, GlobalAggregatesWithNull) {
  // x = [10, 20, 30, NULL, 40]
  const std::array<TypeId, 1> t{TypeId::kInt32};
  DataChunk c;
  c.Initialize(t);
  const std::int32_t xs[5] = {10, 20, 30, 0, 40};
  for (std::size_t i = 0; i < 5; ++i) c.column(0).Set<std::int32_t>(i, xs[i]);
  c.column(0).SetNull(3);
  c.SetSize(5);

  ResultCollector out(SchemaOf({TypeId::kInt64, TypeId::kInt64, TypeId::kInt64, TypeId::kInt32,
                                TypeId::kInt32, TypeId::kDouble}));
  HashAggregate agg({}, {{AggFunc::kCountStar},
                         {AggFunc::kCount, 0, TypeId::kInt32},
                         {AggFunc::kSum, 0, TypeId::kInt32},
                         {AggFunc::kMin, 0, TypeId::kInt32},
                         {AggFunc::kMax, 0, TypeId::kInt32},
                         {AggFunc::kAvg, 0, TypeId::kInt32}},
                    out);
  agg.Consume(c);
  agg.Finalize();

  ASSERT_EQ(out.row_count(), 1u);
  EXPECT_EQ(out.Get<std::int64_t>(0, 0), 5);   // COUNT(*)
  EXPECT_EQ(out.Get<std::int64_t>(0, 1), 4);   // COUNT(x): NULL skipped
  EXPECT_EQ(out.Get<std::int64_t>(0, 2), 100); // SUM
  EXPECT_EQ(out.Get<std::int32_t>(0, 3), 10);  // MIN
  EXPECT_EQ(out.Get<std::int32_t>(0, 4), 40);  // MAX
  EXPECT_DOUBLE_EQ(out.Get<double>(0, 5), 25.0);  // AVG = 100/4
}

TEST(HashAggregate, GlobalOverEmptyInput) {
  ResultCollector out(SchemaOf({TypeId::kInt64, TypeId::kInt64, TypeId::kInt32}));
  HashAggregate agg({}, {{AggFunc::kCountStar},
                         {AggFunc::kSum, 0, TypeId::kInt32},
                         {AggFunc::kMin, 0, TypeId::kInt32}},
                    out);
  agg.Finalize();  // no Consume at all
  ASSERT_EQ(out.row_count(), 1u);
  EXPECT_EQ(out.Get<std::int64_t>(0, 0), 0);  // COUNT(*) of nothing = 0
  EXPECT_TRUE(out.IsNull(0, 1));              // SUM of nothing = NULL
  EXPECT_TRUE(out.IsNull(0, 2));              // MIN of nothing = NULL
}

TEST(HashAggregate, GroupBySingleKey) {
  // key=[1,2,1,2,1], val=[10,20,30,40,50]
  const std::array<TypeId, 2> t{TypeId::kInt32, TypeId::kInt32};
  DataChunk c;
  c.Initialize(t);
  const std::int32_t keys[5] = {1, 2, 1, 2, 1};
  const std::int32_t vals[5] = {10, 20, 30, 40, 50};
  for (std::size_t i = 0; i < 5; ++i) {
    c.column(0).Set<std::int32_t>(i, keys[i]);
    c.column(1).Set<std::int32_t>(i, vals[i]);
  }
  c.SetSize(5);

  ResultCollector out(SchemaOf({TypeId::kInt32, TypeId::kInt64, TypeId::kInt64}));
  HashAggregate agg({{0, TypeId::kInt32}},
                    {{AggFunc::kSum, 1, TypeId::kInt32}, {AggFunc::kCountStar}}, out);
  agg.Consume(c);
  agg.Finalize();

  ASSERT_EQ(out.row_count(), 2u);
  const auto g1 = FindI32(out, 0, 1);
  const auto g2 = FindI32(out, 0, 2);
  ASSERT_TRUE(g1 && g2);
  EXPECT_EQ(out.Get<std::int64_t>(*g1, 1), 90);  // 10+30+50
  EXPECT_EQ(out.Get<std::int64_t>(*g1, 2), 3);
  EXPECT_EQ(out.Get<std::int64_t>(*g2, 1), 60);  // 20+40
  EXPECT_EQ(out.Get<std::int64_t>(*g2, 2), 2);
}

TEST(HashAggregate, NullGroupKeyIsItsOwnGroup) {
  // key=[1,NULL,1,NULL], val=[5,6,7,8]
  const std::array<TypeId, 2> t{TypeId::kInt32, TypeId::kInt32};
  DataChunk c;
  c.Initialize(t);
  const std::int32_t vals[4] = {5, 6, 7, 8};
  for (std::size_t i = 0; i < 4; ++i) c.column(1).Set<std::int32_t>(i, vals[i]);
  c.column(0).Set<std::int32_t>(0, 1);
  c.column(0).SetNull(1);
  c.column(0).Set<std::int32_t>(2, 1);
  c.column(0).SetNull(3);
  c.SetSize(4);

  ResultCollector out(SchemaOf({TypeId::kInt32, TypeId::kInt64}));
  HashAggregate agg({{0, TypeId::kInt32}}, {{AggFunc::kSum, 1, TypeId::kInt32}}, out);
  agg.Consume(c);
  agg.Finalize();

  ASSERT_EQ(out.row_count(), 2u);
  const auto g1 = FindI32(out, 0, 1);
  ASSERT_TRUE(g1);
  EXPECT_EQ(out.Get<std::int64_t>(*g1, 1), 12);  // 5+7
  // the other group is the NULL-key group
  std::size_t gnull = (*g1 == 0) ? 1 : 0;
  EXPECT_TRUE(out.IsNull(gnull, 0));
  EXPECT_EQ(out.Get<std::int64_t>(gnull, 1), 14);  // 6+8
}

TEST(HashAggregate, AllNullAggregatedGroup) {
  // key=[1,1,2,2], val=[10,NULL,NULL,NULL]
  const std::array<TypeId, 2> t{TypeId::kInt32, TypeId::kInt32};
  DataChunk c;
  c.Initialize(t);
  const std::int32_t keys[4] = {1, 1, 2, 2};
  for (std::size_t i = 0; i < 4; ++i) c.column(0).Set<std::int32_t>(i, keys[i]);
  c.column(1).Set<std::int32_t>(0, 10);
  c.column(1).SetNull(1);
  c.column(1).SetNull(2);
  c.column(1).SetNull(3);
  c.SetSize(4);

  ResultCollector out(
      SchemaOf({TypeId::kInt32, TypeId::kInt64, TypeId::kInt64, TypeId::kInt64, TypeId::kInt32}));
  HashAggregate agg({{0, TypeId::kInt32}},
                    {{AggFunc::kSum, 1, TypeId::kInt32},
                     {AggFunc::kCount, 1, TypeId::kInt32},
                     {AggFunc::kCountStar},
                     {AggFunc::kMin, 1, TypeId::kInt32}},
                    out);
  agg.Consume(c);
  agg.Finalize();

  const auto g1 = FindI32(out, 0, 1);
  const auto g2 = FindI32(out, 0, 2);
  ASSERT_TRUE(g1 && g2);
  EXPECT_EQ(out.Get<std::int64_t>(*g1, 1), 10);  // SUM
  EXPECT_EQ(out.Get<std::int64_t>(*g1, 2), 1);   // COUNT(val)
  EXPECT_EQ(out.Get<std::int64_t>(*g1, 3), 2);   // COUNT(*)
  EXPECT_EQ(out.Get<std::int32_t>(*g1, 4), 10);  // MIN
  EXPECT_TRUE(out.IsNull(*g2, 1));               // SUM of all-NULL = NULL
  EXPECT_EQ(out.Get<std::int64_t>(*g2, 2), 0);   // COUNT(val) = 0
  EXPECT_EQ(out.Get<std::int64_t>(*g2, 3), 2);   // COUNT(*) = 2 (counts rows, not values)
  EXPECT_TRUE(out.IsNull(*g2, 4));               // MIN of all-NULL = NULL
}

TEST(HashAggregate, SumInt32WidensToInt64NoOverflow) {
  // 1000 * 3,000,000 = 3,000,000,000 > INT32_MAX (2,147,483,647).
  const std::array<TypeId, 1> t{TypeId::kInt32};
  DataChunk c;
  c.Initialize(t);
  const std::size_t n = 1000;
  for (std::size_t i = 0; i < n; ++i) c.column(0).Set<std::int32_t>(i, 3'000'000);
  c.SetSize(n);

  ResultCollector out(SchemaOf({TypeId::kInt64}));
  HashAggregate agg({}, {{AggFunc::kSum, 0, TypeId::kInt32}}, out);
  agg.Consume(c);
  agg.Finalize();
  ASSERT_EQ(out.row_count(), 1u);
  EXPECT_EQ(out.Get<std::int64_t>(0, 0), 3'000'000'000LL);  // int32 accumulator would wrap
}

TEST(HashAggregate, MultiKeyGroupBy) {
  // (a,b) = (1,1),(1,2),(2,1),(1,1)  => groups {1,1}x2, {1,2}, {2,1}
  const std::array<TypeId, 2> t{TypeId::kInt32, TypeId::kInt32};
  DataChunk c;
  c.Initialize(t);
  const std::int32_t a[4] = {1, 1, 2, 1};
  const std::int32_t b[4] = {1, 2, 1, 1};
  for (std::size_t i = 0; i < 4; ++i) {
    c.column(0).Set<std::int32_t>(i, a[i]);
    c.column(1).Set<std::int32_t>(i, b[i]);
  }
  c.SetSize(4);

  ResultCollector out(SchemaOf({TypeId::kInt32, TypeId::kInt32, TypeId::kInt64}));
  HashAggregate agg({{0, TypeId::kInt32}, {1, TypeId::kInt32}}, {{AggFunc::kCountStar}}, out);
  agg.Consume(c);
  agg.Finalize();
  EXPECT_EQ(out.row_count(), 3u);
  EXPECT_EQ(agg.group_count(), 3u);
}

TEST(HashAggregate, ManyGroupsGrowAndMultiChunkResult) {
  // 5000 distinct keys across three input chunks -> exercises Grow() and a
  // result that spans multiple output chunks.
  ResultCollector out(SchemaOf({TypeId::kInt32, TypeId::kInt64}));
  HashAggregate agg({{0, TypeId::kInt32}}, {{AggFunc::kCountStar}}, out);

  std::int32_t next = 0;
  for (const std::size_t batch : {2048u, 2048u, 904u}) {
    const std::array<TypeId, 1> t{TypeId::kInt32};
    DataChunk c;
    c.Initialize(t);
    for (std::size_t i = 0; i < batch; ++i) c.column(0).Set<std::int32_t>(i, next++);
    c.SetSize(batch);
    agg.Consume(c);
  }
  agg.Finalize();

  EXPECT_EQ(agg.group_count(), 5000u);
  EXPECT_EQ(out.row_count(), 5000u);
  const auto g = FindI32(out, 0, 4321);
  ASSERT_TRUE(g);
  EXPECT_EQ(out.Get<std::int64_t>(*g, 1), 1);  // each key appears once
}

}  // namespace
}  // namespace strata
