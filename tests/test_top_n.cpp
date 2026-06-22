#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

#include "strata/data/data_chunk.hpp"
#include "strata/exec/result_collector.hpp"
#include "strata/exec/sort.hpp"
#include "strata/exec/top_n.hpp"
#include "strata/storage/columnar_table.hpp"

namespace strata {
namespace {

Schema SchemaOf(std::initializer_list<TypeId> types) {
  std::vector<ColumnDef> cols;
  int i = 0;
  for (const TypeId t : types) cols.push_back({"c" + std::to_string(i++), t});
  return Schema(std::move(cols));
}

DataChunk MakeI32(const std::vector<std::int32_t>& vals) {
  const std::array<TypeId, 1> t{TypeId::kInt32};
  DataChunk c;
  c.Initialize(t);
  for (std::size_t i = 0; i < vals.size(); ++i) c.column(0).Set<std::int32_t>(i, vals[i]);
  c.SetSize(vals.size());
  return c;
}

// Feed a large value vector through `sink` in <= kVectorSize chunks.
void Feed(Sink& sink, const std::vector<std::int32_t>& vals) {
  for (std::size_t off = 0; off < vals.size(); off += kVectorSize) {
    const std::size_t len = std::min(kVectorSize, vals.size() - off);
    sink.Consume(MakeI32(std::vector<std::int32_t>(vals.begin() + static_cast<std::ptrdiff_t>(off),
                                                   vals.begin() + static_cast<std::ptrdiff_t>(off + len))));
  }
}

TEST(TopN, EqualsFullSortPrefix) {
  std::mt19937 rng(42);
  std::vector<std::int32_t> vals(5000);
  for (auto& x : vals) x = static_cast<std::int32_t>(rng() % 100000);

  const std::size_t k = 17;
  ResultCollector full(SchemaOf({TypeId::kInt32}));
  Sort sort({SortKey{0, TypeId::kInt32, true, false}}, full);
  ResultCollector topn_out(SchemaOf({TypeId::kInt32}));
  TopN topn({SortKey{0, TypeId::kInt32, true, false}}, k, topn_out);

  Feed(sort, vals);
  Feed(topn, vals);
  sort.Finalize();
  topn.Finalize();

  ASSERT_EQ(topn_out.row_count(), k);
  ASSERT_EQ(full.row_count(), 5000u);
  for (std::size_t j = 0; j < k; ++j) {
    EXPECT_EQ(topn_out.Get<std::int32_t>(j, 0), full.Get<std::int32_t>(j, 0)) << "j=" << j;
  }
}

TEST(TopN, DescEqualsFullSortPrefix) {
  std::mt19937 rng(7);
  std::vector<std::int32_t> vals(3000);
  for (auto& x : vals) x = static_cast<std::int32_t>(rng() % 1000);

  const std::size_t k = 25;
  ResultCollector full(SchemaOf({TypeId::kInt32}));
  Sort sort({SortKey{0, TypeId::kInt32, false, false}}, full);
  ResultCollector topn_out(SchemaOf({TypeId::kInt32}));
  TopN topn({SortKey{0, TypeId::kInt32, false, false}}, k, topn_out);
  Feed(sort, vals);
  Feed(topn, vals);
  sort.Finalize();
  topn.Finalize();
  ASSERT_EQ(topn_out.row_count(), k);
  for (std::size_t j = 0; j < k; ++j) {
    EXPECT_EQ(topn_out.Get<std::int32_t>(j, 0), full.Get<std::int32_t>(j, 0)) << "j=" << j;
  }
}

TEST(TopN, KGreaterThanInputReturnsAllSorted) {
  ResultCollector out(SchemaOf({TypeId::kInt32}));
  TopN topn({SortKey{0, TypeId::kInt32, true, false}}, 100, out);
  topn.Consume(MakeI32({5, 2, 8, 1, 9}));
  topn.Finalize();
  ASSERT_EQ(out.row_count(), 5u);
  EXPECT_EQ(out.Get<std::int32_t>(0, 0), 1);
  EXPECT_EQ(out.Get<std::int32_t>(4, 0), 9);
}

TEST(TopN, ZeroK) {
  ResultCollector out(SchemaOf({TypeId::kInt32}));
  TopN topn({SortKey{0, TypeId::kInt32, true, false}}, 0, out);
  topn.Consume(MakeI32({5, 2, 8}));
  topn.Finalize();
  EXPECT_EQ(out.row_count(), 0u);
}

TEST(TopN, TiesAtBoundary) {
  ResultCollector out(SchemaOf({TypeId::kInt32}));
  TopN topn({SortKey{0, TypeId::kInt32, true, false}}, 2, out);
  topn.Consume(MakeI32({5, 5, 5, 1, 1}));
  topn.Finalize();
  ASSERT_EQ(out.row_count(), 2u);
  EXPECT_EQ(out.Get<std::int32_t>(0, 0), 1);
  EXPECT_EQ(out.Get<std::int32_t>(1, 0), 1);
}

}  // namespace
}  // namespace strata
