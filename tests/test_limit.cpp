#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "strata/data/data_chunk.hpp"
#include "strata/exec/limit.hpp"
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

DataChunk Seq(std::int32_t start, std::size_t n) {
  const std::array<TypeId, 1> t{TypeId::kInt32};
  DataChunk c;
  c.Initialize(t);
  for (std::size_t i = 0; i < n; ++i) c.column(0).Set<std::int32_t>(i, start + static_cast<std::int32_t>(i));
  c.SetSize(n);
  return c;
}

TEST(Limit, FirstN) {
  ResultCollector out(SchemaOf({TypeId::kInt32}));
  Limit lim(3, out);
  lim.Consume(Seq(0, 10));
  lim.Finalize();
  ASSERT_EQ(out.row_count(), 3u);
  EXPECT_EQ(out.Get<std::int32_t>(0, 0), 0);
  EXPECT_EQ(out.Get<std::int32_t>(2, 0), 2);
}

TEST(Limit, NGreaterThanInput) {
  ResultCollector out(SchemaOf({TypeId::kInt32}));
  Limit lim(100, out);
  lim.Consume(Seq(0, 5));
  lim.Finalize();
  EXPECT_EQ(out.row_count(), 5u);
}

TEST(Limit, Zero) {
  ResultCollector out(SchemaOf({TypeId::kInt32}));
  Limit lim(0, out);
  lim.Consume(Seq(0, 5));
  lim.Finalize();
  EXPECT_EQ(out.row_count(), 0u);
}

TEST(Limit, SpansChunks) {
  // limit 5 across two 4-row chunks -> 4 from first, 1 from second
  ResultCollector out(SchemaOf({TypeId::kInt32}));
  Limit lim(5, out);
  lim.Consume(Seq(0, 4));
  lim.Consume(Seq(100, 4));
  lim.Finalize();
  ASSERT_EQ(out.row_count(), 5u);
  EXPECT_EQ(out.Get<std::int32_t>(3, 0), 3);
  EXPECT_EQ(out.Get<std::int32_t>(4, 0), 100);  // first row of second chunk
}

}  // namespace
}  // namespace strata
