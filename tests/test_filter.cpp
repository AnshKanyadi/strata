#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>

#include "strata/data/data_chunk.hpp"
#include "strata/data/selection_vector.hpp"
#include "strata/exec/expression.hpp"
#include "strata/exec/filter.hpp"

namespace strata {
namespace {

DataChunk SeqChunk(std::size_t n) {  // column 0 = [0, 1, ..., n-1]
  const std::array<TypeId, 1> t{TypeId::kInt32};
  DataChunk c;
  c.Initialize(t);
  for (std::size_t i = 0; i < n; ++i) c.column(0).Set<std::int32_t>(i, static_cast<std::int32_t>(i));
  c.SetSize(n);
  return c;
}

ExprPtr Pred(simd::CmpOp op, std::int32_t k) {
  return Expression::Comparison(op, Expression::ColumnRef(0, TypeId::kInt32),
                                Expression::Constant(Value::Int32(k)));
}

TEST(Filter, AllPass) {  // 100% selectivity
  const DataChunk c = SeqChunk(100);
  Filter f(Pred(simd::CmpOp::kGe, 0));
  SelectionVector sel;
  EXPECT_EQ(f.Select(c, sel), 100u);
  EXPECT_EQ(sel.Get(0), 0u);
  EXPECT_EQ(sel.Get(99), 99u);
}

TEST(Filter, NonePass) {  // 0% selectivity
  const DataChunk c = SeqChunk(100);
  Filter f(Pred(simd::CmpOp::kLt, 0));
  SelectionVector sel;
  EXPECT_EQ(f.Select(c, sel), 0u);
}

TEST(Filter, EveryOther) {  // 50%: column [1,0,1,0,...], predicate == 1
  const std::array<TypeId, 1> t{TypeId::kInt32};
  DataChunk c;
  c.Initialize(t);
  const std::size_t n = 100;
  for (std::size_t i = 0; i < n; ++i) c.column(0).Set<std::int32_t>(i, (i % 2 == 0) ? 1 : 0);
  c.SetSize(n);
  Filter f(Pred(simd::CmpOp::kEq, 1));
  SelectionVector sel;
  EXPECT_EQ(f.Select(c, sel), 50u);
  EXPECT_EQ(sel.Get(0), 0u);
  EXPECT_EQ(sel.Get(1), 2u);
  EXPECT_EQ(sel.Get(49), 98u);
}

TEST(Filter, NullsAreExcluded) {  // 3VL WHERE: NULL predicate does not pass
  const std::array<TypeId, 1> t{TypeId::kInt32};
  DataChunk c;
  c.Initialize(t);
  c.column(0).Set<std::int32_t>(0, 1);
  c.column(0).SetNull(1);
  c.column(0).Set<std::int32_t>(2, 5);
  c.column(0).SetNull(3);
  c.column(0).Set<std::int32_t>(4, 7);
  c.SetSize(5);
  Filter f(Pred(simd::CmpOp::kGt, 0));  // a > 0
  SelectionVector sel;
  EXPECT_EQ(f.Select(c, sel), 3u);  // only the three non-NULL rows
  EXPECT_EQ(sel.Get(0), 0u);
  EXPECT_EQ(sel.Get(1), 2u);
  EXPECT_EQ(sel.Get(2), 4u);
}

TEST(Filter, FullChunkWithKernelTail) {
  // 2045 is not a multiple of any SIMD lane width (4/8 for i32), so the
  // comparison kernel exercises its scalar tail. (A chunk holds <= kVectorSize.)
  const DataChunk c = SeqChunk(2045);
  Filter f(Pred(simd::CmpOp::kLt, 100));
  SelectionVector sel;
  EXPECT_EQ(f.Select(c, sel), 100u);  // values 0..99 pass
}

}  // namespace
}  // namespace strata
