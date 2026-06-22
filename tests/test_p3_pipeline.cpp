#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <sstream>
#include <utility>
#include <vector>

#include "strata/data/data_chunk.hpp"
#include "strata/data/selection_vector.hpp"
#include "strata/exec/expression.hpp"
#include "strata/exec/filter.hpp"
#include "strata/exec/project.hpp"
#include "strata/exec/result_collector.hpp"
#include "strata/exec/scan.hpp"
#include "strata/storage/csv_loader.hpp"

namespace strata {
namespace {

// End-to-end: load -> Scan -> Filter (WHERE val > 10) -> Project (id, val+val)
// -> ResultCollector, threading a selection vector through, deep-copying at the
// sink. This is the full P3 dataflow.
TEST(P3Pipeline, FilterThenProject) {
  const Schema schema({{"id", TypeId::kInt32}, {"val", TypeId::kInt32}});
  std::istringstream csv("1,10\n2,25\n3,5\n4,40\n");
  auto tr = LoadDelimited(csv, schema, CsvOptions());
  ASSERT_TRUE(tr.has_value());
  const ColumnarTable table = std::move(*tr);

  Filter filter(Expression::Comparison(simd::CmpOp::kGt,
                                        Expression::ColumnRef(1, TypeId::kInt32),
                                        Expression::Constant(Value::Int32(10))));
  std::vector<ExprPtr> projs;
  projs.push_back(Expression::ColumnRef(0, TypeId::kInt32));  // id
  projs.push_back(Expression::Arithmetic(simd::ArithOp::kAdd,
                                         Expression::ColumnRef(1, TypeId::kInt32),
                                         Expression::ColumnRef(1, TypeId::kInt32)));  // val + val
  Project project(std::move(projs));

  const Schema out_schema({{"id", TypeId::kInt32}, {"v2", TypeId::kInt32}});
  ResultCollector collector(out_schema);

  Scan scan(table);
  while (const DataChunk* chunk = scan.GetChunk()) {
    SelectionVector sel;
    const std::size_t count = filter.Select(*chunk, sel);
    DataChunk out;
    out.Initialize(project.output_types(), chunk->capacity());
    project.Execute(*chunk, sel, count, out);
    collector.Consume(out);
  }
  collector.Finalize();

  ASSERT_EQ(collector.row_count(), 2u);          // val > 10: (2,25) and (4,40)
  EXPECT_EQ(collector.Get<std::int32_t>(0, 0), 2);
  EXPECT_EQ(collector.Get<std::int32_t>(0, 1), 50);  // 25 + 25
  EXPECT_EQ(collector.Get<std::int32_t>(1, 0), 4);
  EXPECT_EQ(collector.Get<std::int32_t>(1, 1), 80);  // 40 + 40
}

}  // namespace
}  // namespace strata
