#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include "strata/data/data_chunk.hpp"
#include "strata/data/selection_vector.hpp"
#include "strata/exec/expression.hpp"
#include "strata/exec/project.hpp"

namespace strata {
namespace {

TEST(Project, GathersSelectedRowsAndEvaluates) {
  // input a = [10, 20, 30, 40]; select rows {1, 3}; project [a, a + a].
  const std::array<TypeId, 1> t{TypeId::kInt32};
  DataChunk in;
  in.Initialize(t);
  for (std::size_t i = 0; i < 4; ++i) {
    in.column(0).Set<std::int32_t>(i, static_cast<std::int32_t>((i + 1) * 10));
  }
  in.SetSize(4);

  SelectionVector sel(2);
  sel.Set(0, 1);
  sel.Set(1, 3);

  std::vector<ExprPtr> exprs;
  exprs.push_back(Expression::ColumnRef(0, TypeId::kInt32));
  exprs.push_back(Expression::Arithmetic(simd::ArithOp::kAdd,
                                          Expression::ColumnRef(0, TypeId::kInt32),
                                          Expression::ColumnRef(0, TypeId::kInt32)));
  Project p(std::move(exprs));
  ASSERT_EQ(p.column_count(), 2u);

  DataChunk out;
  out.Initialize(p.output_types());
  p.Execute(in, sel, 2, out);

  ASSERT_EQ(out.size(), 2u);
  EXPECT_EQ(out.column(0).Get<std::int32_t>(0), 20);
  EXPECT_EQ(out.column(0).Get<std::int32_t>(1), 40);
  EXPECT_EQ(out.column(1).Get<std::int32_t>(0), 40);  // 20 + 20
  EXPECT_EQ(out.column(1).Get<std::int32_t>(1), 80);  // 40 + 40
}

TEST(Project, GathersNulls) {
  // a = [5, NULL, 9]; select all; project [a].
  const std::array<TypeId, 1> t{TypeId::kInt32};
  DataChunk in;
  in.Initialize(t);
  in.column(0).Set<std::int32_t>(0, 5);
  in.column(0).SetNull(1);
  in.column(0).Set<std::int32_t>(2, 9);
  in.SetSize(3);

  SelectionVector sel(3);
  sel.Set(0, 0);
  sel.Set(1, 1);
  sel.Set(2, 2);

  std::vector<ExprPtr> exprs;
  exprs.push_back(Expression::ColumnRef(0, TypeId::kInt32));
  Project p(std::move(exprs));
  DataChunk out;
  out.Initialize(p.output_types());
  p.Execute(in, sel, 3, out);

  EXPECT_EQ(out.column(0).Get<std::int32_t>(0), 5);
  EXPECT_TRUE(out.column(0).IsNull(1));
  EXPECT_EQ(out.column(0).Get<std::int32_t>(2), 9);
}

TEST(Project, VarcharGatherDeepCopies) {
  const std::array<TypeId, 1> t{TypeId::kVarchar};
  DataChunk in;
  in.Initialize(t);
  in.column(0).Set<StringRef>(0, in.column(0).AddString("this is a long string value"));
  in.column(0).Set<StringRef>(1, in.column(0).AddString("short"));
  in.SetSize(2);

  SelectionVector sel(1);
  sel.Set(0, 0);  // pick the long string

  std::vector<ExprPtr> exprs;
  exprs.push_back(Expression::ColumnRef(0, TypeId::kVarchar));
  Project p(std::move(exprs));
  DataChunk out;
  out.Initialize(p.output_types());
  p.Execute(in, sel, 1, out);
  EXPECT_EQ(out.column(0).Get<StringRef>(0).view(),
            std::string_view{"this is a long string value"});
}

}  // namespace
}  // namespace strata
