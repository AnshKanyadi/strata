#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <string_view>

#include "strata/data/data_chunk.hpp"
#include "strata/exec/expression.hpp"

namespace strata {
namespace {

// Three-valued helpers for the logical-op truth tables.
enum class TV { kT, kF, kN };
void SetTV(Vector& col, std::size_t i, TV v) {
  if (v == TV::kN) col.SetNull(i);
  else col.Set<std::uint8_t>(i, v == TV::kT ? 1 : 0);
}
TV GetTV(const Vector& col, std::size_t i) {
  if (col.IsNull(i)) return TV::kN;
  return col.Get<std::uint8_t>(i) != 0 ? TV::kT : TV::kF;
}

TEST(Expression, ComparisonNullPropagates) {
  // a = [1, NULL, 5]; a > 3  =>  [FALSE, NULL, TRUE]
  const std::array<TypeId, 1> types{TypeId::kInt32};
  DataChunk c;
  c.Initialize(types);
  c.column(0).Set<std::int32_t>(0, 1);
  c.column(0).SetNull(1);
  c.column(0).Set<std::int32_t>(2, 5);
  c.SetSize(3);

  ExprPtr e = Expression::Comparison(simd::CmpOp::kGt, Expression::ColumnRef(0, TypeId::kInt32),
                                     Expression::Constant(Value::Int32(3)));
  ExpressionExecutor ex;
  const Vector r = ex.Execute(*e, c);
  EXPECT_EQ(r.type(), TypeId::kBool);
  EXPECT_EQ(int{r.Get<std::uint8_t>(0)}, 0);
  EXPECT_TRUE(r.IsNull(1));  // NULL > 3 is NULL (unknown), NOT false
  EXPECT_EQ(int{r.Get<std::uint8_t>(2)}, 1);
}

TEST(Expression, ArithmeticNullPropagates) {
  // a = [10, NULL, 3], b = [1, 2, 3]; a + b => [11, NULL, 6]
  const std::array<TypeId, 2> types{TypeId::kInt32, TypeId::kInt32};
  DataChunk c;
  c.Initialize(types);
  c.column(0).Set<std::int32_t>(0, 10);
  c.column(0).SetNull(1);
  c.column(0).Set<std::int32_t>(2, 3);
  c.column(1).Set<std::int32_t>(0, 1);
  c.column(1).Set<std::int32_t>(1, 2);
  c.column(1).Set<std::int32_t>(2, 3);
  c.SetSize(3);

  ExprPtr e = Expression::Arithmetic(simd::ArithOp::kAdd, Expression::ColumnRef(0, TypeId::kInt32),
                                     Expression::ColumnRef(1, TypeId::kInt32));
  ExpressionExecutor ex;
  const Vector r = ex.Execute(*e, c);
  EXPECT_EQ(r.Get<std::int32_t>(0), 11);
  EXPECT_TRUE(r.IsNull(1));
  EXPECT_EQ(r.Get<std::int32_t>(2), 6);
}

// Build a chunk of two BOOL columns from the standard 9-row T/F/N cross product.
DataChunk MakeLogicChunk() {
  const std::array<TypeId, 2> types{TypeId::kBool, TypeId::kBool};
  DataChunk c;
  c.Initialize(types);
  const TV l[9] = {TV::kT, TV::kT, TV::kT, TV::kF, TV::kF, TV::kF, TV::kN, TV::kN, TV::kN};
  const TV r[9] = {TV::kT, TV::kF, TV::kN, TV::kT, TV::kF, TV::kN, TV::kT, TV::kF, TV::kN};
  for (std::size_t i = 0; i < 9; ++i) {
    SetTV(c.column(0), i, l[i]);
    SetTV(c.column(1), i, r[i]);
  }
  c.SetSize(9);
  return c;
}

TEST(Expression, ThreeValuedAnd) {
  const DataChunk c = MakeLogicChunk();
  ExprPtr e = Expression::And(Expression::ColumnRef(0, TypeId::kBool),
                              Expression::ColumnRef(1, TypeId::kBool));
  ExpressionExecutor ex;
  const Vector r = ex.Execute(*e, c);
  // l: T T T F F F N N N ; r: T F N T F N T F N
  const TV want[9] = {TV::kT, TV::kF, TV::kN, TV::kF, TV::kF, TV::kF, TV::kN, TV::kF, TV::kN};
  for (std::size_t i = 0; i < 9; ++i) EXPECT_EQ(GetTV(r, i) == want[i], true) << "row " << i;
}

TEST(Expression, ThreeValuedOr) {
  const DataChunk c = MakeLogicChunk();
  ExprPtr e = Expression::Or(Expression::ColumnRef(0, TypeId::kBool),
                             Expression::ColumnRef(1, TypeId::kBool));
  ExpressionExecutor ex;
  const Vector r = ex.Execute(*e, c);
  const TV want[9] = {TV::kT, TV::kT, TV::kT, TV::kT, TV::kF, TV::kN, TV::kT, TV::kN, TV::kN};
  for (std::size_t i = 0; i < 9; ++i) EXPECT_EQ(GetTV(r, i) == want[i], true) << "row " << i;
}

TEST(Expression, ThreeValuedNot) {
  const std::array<TypeId, 1> types{TypeId::kBool};
  DataChunk c;
  c.Initialize(types);
  SetTV(c.column(0), 0, TV::kT);
  SetTV(c.column(0), 1, TV::kF);
  SetTV(c.column(0), 2, TV::kN);
  c.SetSize(3);
  ExprPtr e = Expression::Not(Expression::ColumnRef(0, TypeId::kBool));
  ExpressionExecutor ex;
  const Vector r = ex.Execute(*e, c);
  EXPECT_EQ(GetTV(r, 0), TV::kF);
  EXPECT_EQ(GetTV(r, 1), TV::kT);
  EXPECT_EQ(GetTV(r, 2), TV::kN);  // NOT NULL is NULL
}

TEST(Expression, Int64AndDoubleArithmetic) {
  const std::array<TypeId, 2> types{TypeId::kInt64, TypeId::kDouble};
  DataChunk c;
  c.Initialize(types);
  c.column(0).Set<std::int64_t>(0, std::int64_t{1} << 40);
  c.column(1).Set<double>(0, 2.5);
  c.SetSize(1);
  ExpressionExecutor ex;
  const Vector r64 = ex.Execute(*Expression::Arithmetic(simd::ArithOp::kMul,
                                                        Expression::ColumnRef(0, TypeId::kInt64),
                                                        Expression::Constant(Value::Int64(3))),
                                c);
  EXPECT_EQ(r64.Get<std::int64_t>(0), (std::int64_t{1} << 40) * 3);
  const Vector rd = ex.Execute(*Expression::Arithmetic(simd::ArithOp::kSub,
                                                       Expression::ColumnRef(1, TypeId::kDouble),
                                                       Expression::Constant(Value::Double(0.5))),
                               c);
  EXPECT_DOUBLE_EQ(rd.Get<double>(0), 2.0);
}

TEST(Expression, StringEquality) {
  const std::array<TypeId, 1> types{TypeId::kVarchar};
  DataChunk c;
  c.Initialize(types);
  c.column(0).Set<StringRef>(0, c.column(0).AddString("AIR"));
  c.column(0).Set<StringRef>(1, c.column(0).AddString("RAIL"));
  c.column(0).Set<StringRef>(2, c.column(0).AddString("AIR"));
  c.SetSize(3);
  ExprPtr e = Expression::Comparison(simd::CmpOp::kEq, Expression::ColumnRef(0, TypeId::kVarchar),
                                     Expression::Constant(Value::Varchar("AIR")));
  ExpressionExecutor ex;
  const Vector r = ex.Execute(*e, c);
  EXPECT_EQ(int{r.Get<std::uint8_t>(0)}, 1);
  EXPECT_EQ(int{r.Get<std::uint8_t>(1)}, 0);
  EXPECT_EQ(int{r.Get<std::uint8_t>(2)}, 1);
}

TEST(Expression, NestedAndOfComparisons) {
  // (a > 2) AND (a < 8) over [1,5,9]  => [F, T, F]
  const std::array<TypeId, 1> types{TypeId::kInt32};
  DataChunk c;
  c.Initialize(types);
  c.column(0).Set<std::int32_t>(0, 1);
  c.column(0).Set<std::int32_t>(1, 5);
  c.column(0).Set<std::int32_t>(2, 9);
  c.SetSize(3);
  ExprPtr e = Expression::And(
      Expression::Comparison(simd::CmpOp::kGt, Expression::ColumnRef(0, TypeId::kInt32),
                             Expression::Constant(Value::Int32(2))),
      Expression::Comparison(simd::CmpOp::kLt, Expression::ColumnRef(0, TypeId::kInt32),
                             Expression::Constant(Value::Int32(8))));
  ExpressionExecutor ex;
  const Vector r = ex.Execute(*e, c);
  EXPECT_EQ(int{r.Get<std::uint8_t>(0)}, 0);
  EXPECT_EQ(int{r.Get<std::uint8_t>(1)}, 1);
  EXPECT_EQ(int{r.Get<std::uint8_t>(2)}, 0);
}

}  // namespace
}  // namespace strata
