#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "strata/data/types.hpp"
#include "strata/exec/aggregate.hpp"   // AggFunc
#include "strata/exec/expression.hpp"  // ExprPtr

namespace strata {

struct OrderItem {
  ExprPtr expr;
  bool ascending = true;
  bool nulls_first = false;
};

struct AggregateItem {
  AggFunc func;
  ExprPtr arg;        // null for COUNT(*)
  std::string alias;  // output name
};

enum class LogicalOp { kGet, kFilter, kProject, kAggregate, kJoin, kOrder, kLimit };

struct LogicalNode;
using PlanPtr = std::unique_ptr<LogicalNode>;

// One node of the logical plan — a tagged struct (relational algebra), decoupled
// from the physical operators. The binder fills out_types/out_names; the
// optimizer rewrites the tree; the executor lowers it to operators. See ADR 0014.
struct LogicalNode {
  LogicalOp op;
  std::vector<PlanPtr> children;

  // Output schema, computed by the binder.
  std::vector<TypeId> out_types;
  std::vector<std::string> out_names;

  // kGet
  std::string table;
  std::vector<std::size_t> read_columns;  // projection-pushdown result (empty => all)

  // kFilter
  ExprPtr predicate;

  // kProject
  std::vector<ExprPtr> projections;
  std::vector<std::string> proj_names;
  bool project_all = false;  // SELECT * — the binder expands to all input columns

  // kAggregate
  std::vector<ExprPtr> group_keys;
  std::vector<AggregateItem> aggregates;

  // kJoin (inner equi-join). After binding, left/right keys reference their own
  // child's columns; left_width is the number of columns from the left child.
  std::vector<ExprPtr> left_keys;
  std::vector<ExprPtr> right_keys;
  std::size_t left_width = 0;

  // kOrder
  std::vector<OrderItem> order;

  // kLimit
  std::size_t limit = 0;
};

inline PlanPtr MakeNode(LogicalOp op) {
  auto n = std::make_unique<LogicalNode>();
  n->op = op;
  return n;
}

}  // namespace strata
