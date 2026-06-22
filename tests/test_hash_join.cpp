#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "strata/data/data_chunk.hpp"
#include "strata/exec/hash_join.hpp"
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

// A 2-column int32 chunk from (a,b) pairs; std::nullopt => NULL.
using Cell = std::optional<std::int32_t>;
DataChunk Make2(const std::vector<std::pair<Cell, Cell>>& rows) {
  const std::array<TypeId, 2> t{TypeId::kInt32, TypeId::kInt32};
  DataChunk c;
  c.Initialize(t);
  for (std::size_t i = 0; i < rows.size(); ++i) {
    if (rows[i].first) c.column(0).Set<std::int32_t>(i, *rows[i].first);
    else c.column(0).SetNull(i);
    if (rows[i].second) c.column(1).Set<std::int32_t>(i, *rows[i].second);
    else c.column(1).SetNull(i);
  }
  c.SetSize(rows.size());
  return c;
}

std::optional<std::size_t> FindByCol(const ResultCollector& c, std::size_t col, std::int32_t v) {
  for (std::size_t r = 0; r < c.row_count(); ++r) {
    if (!c.IsNull(r, col) && c.Get<std::int32_t>(r, col) == v) return r;
  }
  return std::nullopt;
}

TEST(HashJoin, InnerJoinBasic) {
  // build (key,val): (1,100),(2,200),(3,300); probe (key,px): (2,20),(1,10),(5,50),(2,21)
  const std::array<TypeId, 2> bt{TypeId::kInt32, TypeId::kInt32};
  const std::array<TypeId, 2> pt{TypeId::kInt32, TypeId::kInt32};
  JoinHashTable ht(bt, {0});
  ht.Insert(Make2({{1, 100}, {2, 200}, {3, 300}}));

  ResultCollector out(SchemaOf({TypeId::kInt32, TypeId::kInt32, TypeId::kInt32, TypeId::kInt32}));
  HashJoinProbe probe(ht, pt, {0}, out);
  probe.Consume(Make2({{2, 20}, {1, 10}, {5, 50}, {2, 21}}));
  probe.Finalize();

  ASSERT_EQ(out.row_count(), 3u);  // (2,20),(1,10),(2,21) match; (5,50) does not
  // output cols: [probe key, probe px, build key, build val]
  const auto r10 = FindByCol(out, 1, 10);  // the probe px=10 row
  ASSERT_TRUE(r10);
  EXPECT_EQ(out.Get<std::int32_t>(*r10, 0), 1);   // probe key
  EXPECT_EQ(out.Get<std::int32_t>(*r10, 2), 1);   // build key
  EXPECT_EQ(out.Get<std::int32_t>(*r10, 3), 100); // build val
  const auto r21 = FindByCol(out, 1, 21);
  ASSERT_TRUE(r21);
  EXPECT_EQ(out.Get<std::int32_t>(*r21, 3), 200);  // probe key 2 -> build val 200
  EXPECT_FALSE(FindByCol(out, 1, 50));             // 5 had no match
}

TEST(HashJoin, EmptyBuildSideYieldsNoMatches) {
  const std::array<TypeId, 2> bt{TypeId::kInt32, TypeId::kInt32};
  const std::array<TypeId, 2> pt{TypeId::kInt32, TypeId::kInt32};
  JoinHashTable ht(bt, {0});  // nothing inserted
  ResultCollector out(SchemaOf({TypeId::kInt32, TypeId::kInt32, TypeId::kInt32, TypeId::kInt32}));
  HashJoinProbe probe(ht, pt, {0}, out);
  probe.Consume(Make2({{1, 10}, {2, 20}}));
  probe.Finalize();
  EXPECT_EQ(out.row_count(), 0u);
}

TEST(HashJoin, AllMatchAndNoMatch) {
  const std::array<TypeId, 2> bt{TypeId::kInt32, TypeId::kInt32};
  const std::array<TypeId, 2> pt{TypeId::kInt32, TypeId::kInt32};
  JoinHashTable ht(bt, {0});
  ht.Insert(Make2({{1, 100}, {2, 200}}));

  ResultCollector all(SchemaOf({TypeId::kInt32, TypeId::kInt32, TypeId::kInt32, TypeId::kInt32}));
  HashJoinProbe p1(ht, pt, {0}, all);
  p1.Consume(Make2({{1, 10}, {2, 20}, {1, 11}}));  // all match
  p1.Finalize();
  EXPECT_EQ(all.row_count(), 3u);

  ResultCollector none(SchemaOf({TypeId::kInt32, TypeId::kInt32, TypeId::kInt32, TypeId::kInt32}));
  HashJoinProbe p2(ht, pt, {0}, none);
  p2.Consume(Make2({{7, 70}, {8, 80}}));  // none match
  p2.Finalize();
  EXPECT_EQ(none.row_count(), 0u);
}

TEST(HashJoin, FanOutDuplicateBuildKeys) {
  // build has key 1 three times; one probe row with key 1 -> 3 output rows.
  const std::array<TypeId, 2> bt{TypeId::kInt32, TypeId::kInt32};
  const std::array<TypeId, 2> pt{TypeId::kInt32, TypeId::kInt32};
  JoinHashTable ht(bt, {0});
  ht.Insert(Make2({{1, 100}, {1, 101}, {2, 200}, {1, 102}}));

  ResultCollector out(SchemaOf({TypeId::kInt32, TypeId::kInt32, TypeId::kInt32, TypeId::kInt32}));
  HashJoinProbe probe(ht, pt, {0}, out);
  probe.Consume(Make2({{1, 10}}));
  probe.Finalize();
  EXPECT_EQ(out.row_count(), 3u);  // three build rows with key 1
  // each output row has probe key=1, px=10
  for (std::size_t r = 0; r < out.row_count(); ++r) {
    EXPECT_EQ(out.Get<std::int32_t>(r, 0), 1);
    EXPECT_EQ(out.Get<std::int32_t>(r, 1), 10);
    EXPECT_EQ(out.Get<std::int32_t>(r, 2), 1);
  }
}

TEST(HashJoin, NullKeysExcluded) {
  // build (1,100),(NULL,999); probe (1,10),(NULL,20).
  const std::array<TypeId, 2> bt{TypeId::kInt32, TypeId::kInt32};
  const std::array<TypeId, 2> pt{TypeId::kInt32, TypeId::kInt32};
  JoinHashTable ht(bt, {0});
  ht.Insert(Make2({{1, 100}, {std::nullopt, 999}}));

  ResultCollector out(SchemaOf({TypeId::kInt32, TypeId::kInt32, TypeId::kInt32, TypeId::kInt32}));
  HashJoinProbe probe(ht, pt, {0}, out);
  probe.Consume(Make2({{1, 10}, {std::nullopt, 20}}));
  probe.Finalize();
  ASSERT_EQ(out.row_count(), 1u);  // only 1<->1; NULL build excluded, NULL probe skipped
  EXPECT_EQ(out.Get<std::int32_t>(0, 1), 10);
  EXPECT_EQ(out.Get<std::int32_t>(0, 3), 100);
}

TEST(HashJoin, MultiKey) {
  // build keys (col0,col1); join on both. build rows: (1,1),(1,2),(2,1).
  const std::array<TypeId, 2> bt{TypeId::kInt32, TypeId::kInt32};
  const std::array<TypeId, 2> pt{TypeId::kInt32, TypeId::kInt32};
  JoinHashTable ht(bt, {0, 1});
  ht.Insert(Make2({{1, 1}, {1, 2}, {2, 1}}));

  ResultCollector out(SchemaOf({TypeId::kInt32, TypeId::kInt32, TypeId::kInt32, TypeId::kInt32}));
  HashJoinProbe probe(ht, pt, {0, 1}, out);
  probe.Consume(Make2({{1, 2}, {2, 2}, {1, 1}}));  // (1,2) and (1,1) match; (2,2) does not
  probe.Finalize();
  EXPECT_EQ(out.row_count(), 2u);
}

TEST(HashJoin, FanOutAcrossFlushBoundary) {
  // 3000 build rows with key 7 (> kVectorSize), one probe row key 7 -> 3000 rows,
  // forcing a mid-chain flush at kVectorSize.
  const std::array<TypeId, 2> bt{TypeId::kInt32, TypeId::kInt32};
  const std::array<TypeId, 2> pt{TypeId::kInt32, TypeId::kInt32};
  JoinHashTable ht(bt, {0});
  std::vector<std::pair<Cell, Cell>> build;
  for (std::int32_t i = 0; i < 3000; ++i) build.push_back({7, i});
  // build side spans two chunks (<= kVectorSize each)
  using Rows = std::vector<std::pair<Cell, Cell>>;
  ht.Insert(Make2(Rows(build.begin(), build.begin() + 2000)));
  ht.Insert(Make2(Rows(build.begin() + 2000, build.end())));
  ASSERT_EQ(ht.row_count(), 3000u);

  ResultCollector out(SchemaOf({TypeId::kInt32, TypeId::kInt32, TypeId::kInt32, TypeId::kInt32}));
  HashJoinProbe probe(ht, pt, {0}, out);
  probe.Consume(Make2({{7, 70}}));
  probe.Finalize();
  EXPECT_EQ(out.row_count(), 3000u);
}

TEST(HashJoin, BuildViaSinkInterface) {
  const std::array<TypeId, 2> bt{TypeId::kInt32, TypeId::kInt32};
  const std::array<TypeId, 2> pt{TypeId::kInt32, TypeId::kInt32};
  HashJoinBuild build(bt, {0});
  build.Consume(Make2({{1, 100}, {2, 200}}));
  build.Finalize();

  ResultCollector out(SchemaOf({TypeId::kInt32, TypeId::kInt32, TypeId::kInt32, TypeId::kInt32}));
  HashJoinProbe probe(build.table(), pt, {0}, out);
  EXPECT_EQ(probe.output_types().size(), 4u);
  probe.Consume(Make2({{2, 22}}));
  probe.Finalize();
  ASSERT_EQ(out.row_count(), 1u);
  EXPECT_EQ(out.Get<std::int32_t>(0, 3), 200);
}

}  // namespace
}  // namespace strata
