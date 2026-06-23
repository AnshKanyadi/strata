#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "strata/exec/expression.hpp"
#include "strata/plan/binder.hpp"
#include "strata/plan/catalog.hpp"
#include "strata/plan/logical_plan.hpp"
#include "strata/plan/optimizer.hpp"
#include "strata/plan/parser.hpp"
#include "strata/storage/columnar_table.hpp"

namespace strata {
namespace {

Schema Sch(std::initializer_list<std::pair<const char*, TypeId>> cols) {
  std::vector<ColumnDef> v;
  for (const auto& [n, t] : cols) v.push_back({n, t});
  return Schema(std::move(v));
}

const LogicalNode* FindOp(const LogicalNode* n, LogicalOp op) {
  if (n->op == op) return n;
  for (const PlanPtr& c : n->children) {
    if (const LogicalNode* r = FindOp(c.get(), op)) return r;
  }
  return nullptr;
}

// --- parser structure -------------------------------------------------------

TEST(Parser, NonAggregateShape) {
  auto r = ParseSql("SELECT a, b FROM t WHERE a > 5 ORDER BY b LIMIT 10");
  ASSERT_TRUE(r.has_value()) << (r ? "" : r.error().message);
  const LogicalNode* n = r->get();
  ASSERT_EQ(n->op, LogicalOp::kLimit);
  EXPECT_EQ(n->limit, 10u);
  n = n->children[0].get();
  ASSERT_EQ(n->op, LogicalOp::kOrder);
  n = n->children[0].get();
  ASSERT_EQ(n->op, LogicalOp::kProject);
  EXPECT_EQ(n->projections.size(), 2u);
  n = n->children[0].get();
  ASSERT_EQ(n->op, LogicalOp::kFilter);
  n = n->children[0].get();
  ASSERT_EQ(n->op, LogicalOp::kGet);
  EXPECT_EQ(n->table, "t");
}

TEST(Parser, AggregateShape) {
  auto r = ParseSql("SELECT k, sum(v), count(*) FROM t GROUP BY k");
  ASSERT_TRUE(r.has_value()) << (r ? "" : r.error().message);
  const LogicalNode* agg = FindOp(r->get(), LogicalOp::kAggregate);
  ASSERT_NE(agg, nullptr);
  EXPECT_EQ(agg->group_keys.size(), 1u);
  EXPECT_EQ(agg->aggregates.size(), 2u);
}

TEST(Parser, BetweenDesugars) {
  auto r = ParseSql("SELECT a FROM t WHERE a BETWEEN 1 AND 9");
  ASSERT_TRUE(r.has_value());
  const LogicalNode* f = FindOp(r->get(), LogicalOp::kFilter);
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(f->predicate->kind, ExprKind::kAnd);  // >= AND <=
}

TEST(Parser, RejectsGarbage) {
  EXPECT_FALSE(ParseSql("SELECT FROM").has_value());
  EXPECT_FALSE(ParseSql("SELCT a FROM t").has_value());
}

// --- optimizer rules --------------------------------------------------------

TEST(Optimizer, ConstantFolding) {
  ColumnarTable t(Sch({{"a", TypeId::kInt32}}));
  Catalog cat;
  cat.Add("t", t);
  auto r = ParseSql("SELECT a FROM t WHERE a > 1 + 2");
  ASSERT_TRUE(r.has_value());
  PlanPtr plan = std::move(*r);
  ASSERT_TRUE(Bind(*plan, cat).has_value());
  ConstantFold(*plan);
  const LogicalNode* f = FindOp(plan.get(), LogicalOp::kFilter);
  ASSERT_NE(f, nullptr);
  // predicate: a > (1+2) -> a > 3
  ASSERT_EQ(f->predicate->kind, ExprKind::kComparison);
  const Expression& rhs = *f->predicate->children[1];
  ASSERT_EQ(rhs.kind, ExprKind::kConstant);
  EXPECT_EQ(rhs.constant.i, 3);
}

TEST(Optimizer, ProjectionPushdown) {
  ColumnarTable t(Sch({{"a", TypeId::kInt32},
                       {"b", TypeId::kInt32},
                       {"c", TypeId::kInt32},
                       {"d", TypeId::kInt32}}));
  Catalog cat;
  cat.Add("t", t);
  auto r = ParseSql("SELECT a FROM t WHERE c > 0");  // references a (0) and c (2)
  ASSERT_TRUE(r.has_value());
  PlanPtr plan = std::move(*r);
  ASSERT_TRUE(Bind(*plan, cat).has_value());
  Optimize(plan);
  const LogicalNode* g = FindOp(plan.get(), LogicalOp::kGet);
  ASSERT_NE(g, nullptr);
  EXPECT_EQ(g->read_columns, (std::vector<std::size_t>{0, 2}));  // b and d pruned
}

TEST(Optimizer, PredicatePushdownThroughJoin) {
  // Hand-build a BOUND plan: Filter( Join(GetL[x], GetR[y]), x>5 AND y<3 ).
  PlanPtr L = MakeNode(LogicalOp::kGet);
  L->out_types = {TypeId::kInt32};
  L->out_names = {"x"};
  PlanPtr R = MakeNode(LogicalOp::kGet);
  R->out_types = {TypeId::kInt32};
  R->out_names = {"y"};

  PlanPtr join = MakeNode(LogicalOp::kJoin);
  join->left_width = 1;
  join->out_types = {TypeId::kInt32, TypeId::kInt32};
  join->out_names = {"x", "y"};
  join->children.push_back(std::move(L));
  join->children.push_back(std::move(R));

  PlanPtr filter = MakeNode(LogicalOp::kFilter);
  // x is joined-row col 0, y is col 1
  ExprPtr x_gt = Expression::Comparison(simd::CmpOp::kGt, Expression::ColumnRef(0, TypeId::kInt32),
                                        Expression::Constant(Value::Int32(5)));
  ExprPtr y_lt = Expression::Comparison(simd::CmpOp::kLt, Expression::ColumnRef(1, TypeId::kInt32),
                                        Expression::Constant(Value::Int32(3)));
  filter->predicate = Expression::And(std::move(x_gt), std::move(y_lt));
  filter->children.push_back(std::move(join));

  PlanPtr plan = std::move(filter);
  PredicatePushdown(plan);

  // The Filter dissolves; both conjuncts pushed to their sides.
  ASSERT_EQ(plan->op, LogicalOp::kJoin);
  ASSERT_EQ(plan->children[0]->op, LogicalOp::kFilter);  // left: x > 5
  ASSERT_EQ(plan->children[1]->op, LogicalOp::kFilter);  // right: y < 3 (rebased to col 0)
  const Expression& lp = *plan->children[0]->predicate;
  EXPECT_EQ(lp.children[0]->column_index, 0u);  // x, left col 0
  const Expression& rp = *plan->children[1]->predicate;
  EXPECT_EQ(rp.children[0]->column_index, 0u);  // y rebased from col 1 -> col 0 on the right
}

}  // namespace
}  // namespace strata
