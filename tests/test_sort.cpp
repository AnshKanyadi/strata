#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "strata/data/data_chunk.hpp"
#include "strata/exec/result_collector.hpp"
#include "strata/exec/sort.hpp"
#include "strata/storage/columnar_table.hpp"

namespace strata {
namespace {

Schema SchemaOf(std::initializer_list<TypeId> types) {
  std::vector<ColumnDef> cols;
  int i = 0;
  for (const TypeId t : types) cols.push_back({"c" + std::to_string(i++), t});
  return Schema(std::move(cols));
}

using OptI = std::optional<std::int32_t>;

DataChunk MakeI32(const std::vector<OptI>& vals) {
  const std::array<TypeId, 1> t{TypeId::kInt32};
  DataChunk c;
  c.Initialize(t);
  for (std::size_t i = 0; i < vals.size(); ++i) {
    if (vals[i]) c.column(0).Set<std::int32_t>(i, *vals[i]);
    else c.column(0).SetNull(i);
  }
  c.SetSize(vals.size());
  return c;
}

std::vector<OptI> ReadI32(const ResultCollector& c, std::size_t col) {
  std::vector<OptI> v;
  for (std::size_t r = 0; r < c.row_count(); ++r) {
    v.push_back(c.IsNull(r, col) ? OptI{} : OptI{c.Get<std::int32_t>(r, col)});
  }
  return v;
}

TEST(Sort, AscDefaultNullsLast) {  // matches DuckDB default
  ResultCollector out(SchemaOf({TypeId::kInt32}));
  Sort sort({SortKey{0, TypeId::kInt32, true, false}}, out);
  sort.Consume(MakeI32({3, std::nullopt, 1, std::nullopt, 2}));
  sort.Finalize();
  EXPECT_EQ(ReadI32(out, 0), (std::vector<OptI>{1, 2, 3, std::nullopt, std::nullopt}));
}

TEST(Sort, DescDefaultNullsLast) {  // DuckDB DESC default is still NULLS LAST
  ResultCollector out(SchemaOf({TypeId::kInt32}));
  Sort sort({SortKey{0, TypeId::kInt32, false, false}}, out);
  sort.Consume(MakeI32({3, std::nullopt, 1, std::nullopt, 2}));
  sort.Finalize();
  EXPECT_EQ(ReadI32(out, 0), (std::vector<OptI>{3, 2, 1, std::nullopt, std::nullopt}));
}

TEST(Sort, AscNullsFirst) {
  ResultCollector out(SchemaOf({TypeId::kInt32}));
  Sort sort({SortKey{0, TypeId::kInt32, true, true}}, out);
  sort.Consume(MakeI32({3, std::nullopt, 1, std::nullopt, 2}));
  sort.Finalize();
  EXPECT_EQ(ReadI32(out, 0), (std::vector<OptI>{std::nullopt, std::nullopt, 1, 2, 3}));
}

TEST(Sort, MultiColumnAscThenDesc) {
  // (a,b): (1,2),(1,1),(2,5),(1,3); ORDER BY a ASC, b DESC
  const std::array<TypeId, 2> t{TypeId::kInt32, TypeId::kInt32};
  DataChunk c;
  c.Initialize(t);
  const std::int32_t a[4] = {1, 1, 2, 1};
  const std::int32_t b[4] = {2, 1, 5, 3};
  for (std::size_t i = 0; i < 4; ++i) {
    c.column(0).Set<std::int32_t>(i, a[i]);
    c.column(1).Set<std::int32_t>(i, b[i]);
  }
  c.SetSize(4);

  ResultCollector out(SchemaOf({TypeId::kInt32, TypeId::kInt32}));
  Sort sort({SortKey{0, TypeId::kInt32, true, false}, SortKey{1, TypeId::kInt32, false, false}}, out);
  sort.Consume(c);
  sort.Finalize();
  // a=1 by b desc: 3,2,1 then a=2: 5
  EXPECT_EQ(ReadI32(out, 0), (std::vector<OptI>{1, 1, 1, 2}));
  EXPECT_EQ(ReadI32(out, 1), (std::vector<OptI>{3, 2, 1, 5}));
}

TEST(Sort, IsStable) {
  // (key,tag): (1,0),(1,1),(2,2),(1,3),(2,4); ORDER BY key -> equal keys keep input order
  const std::array<TypeId, 2> t{TypeId::kInt32, TypeId::kInt32};
  DataChunk c;
  c.Initialize(t);
  const std::int32_t key[5] = {1, 1, 2, 1, 2};
  for (std::size_t i = 0; i < 5; ++i) {
    c.column(0).Set<std::int32_t>(i, key[i]);
    c.column(1).Set<std::int32_t>(i, static_cast<std::int32_t>(i));  // tag
  }
  c.SetSize(5);

  ResultCollector out(SchemaOf({TypeId::kInt32, TypeId::kInt32}));
  Sort sort({SortKey{0, TypeId::kInt32, true, false}}, out);
  sort.Consume(c);
  sort.Finalize();
  EXPECT_EQ(ReadI32(out, 0), (std::vector<OptI>{1, 1, 1, 2, 2}));
  EXPECT_EQ(ReadI32(out, 1), (std::vector<OptI>{0, 1, 3, 2, 4}));  // input order within each key
}

TEST(Sort, Strings) {
  const std::array<TypeId, 1> t{TypeId::kVarchar};
  DataChunk c;
  c.Initialize(t);
  const char* in[4] = {"pear", "apple", "cherry", "banana"};
  for (std::size_t i = 0; i < 4; ++i) c.column(0).Set<StringRef>(i, c.column(0).AddString(in[i]));
  c.SetSize(4);

  ResultCollector out(SchemaOf({TypeId::kVarchar}));
  Sort sort({SortKey{0, TypeId::kVarchar, true, false}}, out);
  sort.Consume(c);
  sort.Finalize();
  ASSERT_EQ(out.row_count(), 4u);
  EXPECT_EQ(out.GetString(0, 0), "apple");
  EXPECT_EQ(out.GetString(1, 0), "banana");
  EXPECT_EQ(out.GetString(2, 0), "cherry");
  EXPECT_EQ(out.GetString(3, 0), "pear");
}

TEST(Sort, MultiChunkInput) {
  // Two chunks (3000 rows total) of descending values -> sorted ascending.
  ResultCollector out(SchemaOf({TypeId::kInt32}));
  Sort sort({SortKey{0, TypeId::kInt32, true, false}}, out);
  std::int32_t v = 3000;
  for (const std::size_t batch : {std::size_t{2000}, std::size_t{1000}}) {
    std::vector<OptI> vals;
    for (std::size_t i = 0; i < batch; ++i) vals.push_back(--v);  // 2999..0 across chunks
    sort.Consume(MakeI32(vals));
  }
  sort.Finalize();
  ASSERT_EQ(out.row_count(), 3000u);
  for (std::size_t r = 0; r < 3000; ++r) {
    EXPECT_EQ(out.Get<std::int32_t>(r, 0), static_cast<std::int32_t>(r));  // 0,1,...,2999
  }
}

}  // namespace
}  // namespace strata
