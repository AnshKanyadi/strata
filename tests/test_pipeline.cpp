#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

#include "strata/exec/pipeline.hpp"
#include "strata/exec/result_collector.hpp"
#include "strata/exec/scan.hpp"
#include "strata/storage/csv_loader.hpp"

namespace strata {
namespace {

// Load CSV text into a table, failing the test (without UB) on a load error.
ColumnarTable LoadOrFail(const std::string& csv, Schema schema) {
  std::istringstream in(csv);
  auto r = LoadDelimited(in, std::move(schema), CsvOptions());
  if (!r.has_value()) {
    ADD_FAILURE() << "load failed: " << r.error().message;
    return ColumnarTable{Schema{}};
  }
  return std::move(*r);
}

// SELECT * FROM t — the P2 done-criterion: load, scan, collect, verify.
TEST(Pipeline, SelectStarEndToEnd) {
  const Schema schema({{"id", TypeId::kInt32},
                       {"name", TypeId::kVarchar},
                       {"active", TypeId::kBool}});
  const ColumnarTable table = LoadOrFail("1,alice,1\n2,bob,0\n3,carol,1\n", schema);

  ResultCollector collector{schema};
  Scan scan(table);
  Pipeline(scan, collector).Run();

  ASSERT_EQ(collector.row_count(), 3u);
  EXPECT_EQ(collector.column_count(), 3u);
  EXPECT_EQ(collector.Get<std::int32_t>(0, 0), 1);
  EXPECT_EQ(collector.GetString(1, 1), "bob");
  EXPECT_EQ(static_cast<int>(collector.Get<std::uint8_t>(2, 2)), 1);
  EXPECT_EQ(static_cast<int>(collector.Get<std::uint8_t>(1, 2)), 0);
}

TEST(Pipeline, NullsSurviveTheRoundTrip) {
  const Schema schema({{"a", TypeId::kInt32}, {"b", TypeId::kVarchar}});
  const ColumnarTable table = LoadOrFail("1,x\n,\n3,z\n", schema);

  ResultCollector collector{schema};
  Scan scan(table);
  Pipeline(scan, collector).Run();

  ASSERT_EQ(collector.row_count(), 3u);
  EXPECT_FALSE(collector.IsNull(0, 0));
  EXPECT_TRUE(collector.IsNull(1, 0));   // empty INT32 field
  EXPECT_TRUE(collector.IsNull(1, 1));   // empty VARCHAR field
  EXPECT_EQ(collector.GetString(2, 1), "z");
}

TEST(Pipeline, MultiChunkScanSpansChunkBoundaries) {
  const Schema schema({{"a", TypeId::kInt32}});
  std::ostringstream gen;
  const int rows = 5000;  // > 2 * kVectorSize -> three stored chunks
  for (int i = 0; i < rows; ++i) gen << i << "\n";
  const ColumnarTable table = LoadOrFail(gen.str(), schema);
  ASSERT_EQ(table.chunk_count(), 3u);

  ResultCollector collector{schema};
  Scan scan(table);
  Pipeline(scan, collector).Run();

  ASSERT_EQ(collector.row_count(), static_cast<std::size_t>(rows));
  EXPECT_EQ(collector.Get<std::int32_t>(0, 0), 0);
  EXPECT_EQ(collector.Get<std::int32_t>(2048, 0), 2048);  // first chunk boundary
  EXPECT_EQ(collector.Get<std::int32_t>(4999, 0), 4999);  // last chunk
}

TEST(Pipeline, PrintFormatsDateAndHeader) {
  const Schema schema({{"id", TypeId::kInt32}, {"d", TypeId::kDate}});
  const ColumnarTable table = LoadOrFail("7,2021-03-15\n", schema);

  ResultCollector collector{schema};
  Scan scan(table);
  Pipeline(scan, collector).Run();

  std::ostringstream os;
  collector.Print(os);
  const std::string out = os.str();
  EXPECT_NE(out.find("id"), std::string::npos);          // header printed
  EXPECT_NE(out.find("2021-03-15"), std::string::npos);  // DATE round-tripped to text
}

TEST(Pipeline, EmptyTableProducesNoRows) {
  const Schema schema({{"a", TypeId::kInt32}});
  const ColumnarTable table = LoadOrFail("", schema);
  ResultCollector collector{schema};
  Scan scan(table);
  Pipeline(scan, collector).Run();
  EXPECT_EQ(collector.row_count(), 0u);
}

}  // namespace
}  // namespace strata
