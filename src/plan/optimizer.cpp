#include "strata/plan/optimizer.hpp"

#include <algorithm>
#include <cstdint>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "strata/exec/expression.hpp"
#include "strata/simd/scalar_kernels.hpp"

namespace strata {
namespace {

// ---- expression utilities ----

void CollectRefs(const Expression& e, std::set<std::size_t>& out) {
  if (e.kind == ExprKind::kColumnRef) out.insert(e.column_index);
  for (const ExprPtr& c : e.children) CollectRefs(*c, out);
}

void RemapRefs(Expression& e, const std::vector<std::size_t>& map) {
  if (e.kind == ExprKind::kColumnRef) e.column_index = map[e.column_index];
  for (ExprPtr& c : e.children) RemapRefs(*c, map);
}

void RebaseRefs(Expression& e, std::size_t sub) {
  if (e.kind == ExprKind::kColumnRef) e.column_index -= sub;
  for (ExprPtr& c : e.children) RebaseRefs(*c, sub);
}

// ---- constant folding ----

ExprPtr FoldedArith(simd::ArithOp op, const Value& a, const Value& b, TypeId t) {
  switch (t) {
    case TypeId::kDouble:
      return Expression::Constant(Value::Double(simd::scalar::ApplyArith<double>(op, a.d, b.d)));
    case TypeId::kInt64: {
      const std::int64_t z = simd::scalar::ApplyArith<std::int64_t>(op, a.i, b.i);
      return Expression::Constant(Value::Int64(z));
    }
    default: {  // int32 / date — wrap at 32 bits to match the executor
      const std::int32_t z = simd::scalar::ApplyArith<std::int32_t>(
          op, static_cast<std::int32_t>(a.i), static_cast<std::int32_t>(b.i));
      Value v = (t == TypeId::kDate) ? Value::Date(z) : Value::Int32(z);
      return Expression::Constant(v);
    }
  }
}

ExprPtr FoldedCmp(simd::CmpOp op, const Value& a, const Value& b, TypeId operand_type) {
  bool r = false;
  switch (operand_type) {
    case TypeId::kDouble: r = simd::scalar::ApplyCmp<double>(op, a.d, b.d) != 0; break;
    case TypeId::kInt64:  r = simd::scalar::ApplyCmp<std::int64_t>(op, a.i, b.i) != 0; break;
    case TypeId::kVarchar: {
      const std::string_view x = a.s, y = b.s;
      switch (op) {
        case simd::CmpOp::kEq: r = (x == y); break;
        case simd::CmpOp::kNe: r = (x != y); break;
        case simd::CmpOp::kLt: r = (x < y);  break;
        case simd::CmpOp::kLe: r = (x <= y); break;
        case simd::CmpOp::kGt: r = (x > y);  break;
        case simd::CmpOp::kGe: r = (x >= y); break;
      }
      break;
    }
    default:  // int32 / date / bool
      r = simd::scalar::ApplyCmp<std::int32_t>(op, static_cast<std::int32_t>(a.i),
                                               static_cast<std::int32_t>(b.i)) != 0;
      break;
  }
  return Expression::Constant(Value::Bool(r));
}

void FoldExpr(ExprPtr& e) {
  for (ExprPtr& c : e->children) FoldExpr(c);
  const bool both_const = e->children.size() == 2 &&
                          e->children[0]->kind == ExprKind::kConstant &&
                          e->children[1]->kind == ExprKind::kConstant;
  if (!both_const) return;
  // Skip if any operand is a typed NULL (folding NULL arithmetic/comparison is 3VL;
  // leave it for the executor to keep semantics identical — ADR 0014).
  if (e->children[0]->constant.is_null || e->children[1]->constant.is_null) return;
  const Value& a = e->children[0]->constant;
  const Value& b = e->children[1]->constant;
  if (e->kind == ExprKind::kArithmetic) {
    e = FoldedArith(e->arith, a, b, e->type);
  } else if (e->kind == ExprKind::kComparison) {
    e = FoldedCmp(e->cmp, a, b, e->children[0]->type);  // operand type
  }
}

void FoldAllExprs(LogicalNode& n) {
  if (n.predicate) FoldExpr(n.predicate);
  for (ExprPtr& p : n.projections) FoldExpr(p);
  for (ExprPtr& g : n.group_keys) FoldExpr(g);
  for (AggregateItem& a : n.aggregates) {
    if (a.arg) FoldExpr(a.arg);
  }
  for (OrderItem& o : n.order) FoldExpr(o.expr);
  for (ExprPtr& k : n.left_keys) FoldExpr(k);
  for (ExprPtr& k : n.right_keys) FoldExpr(k);
  for (PlanPtr& c : n.children) FoldAllExprs(*c);
}

// ---- predicate pushdown ----

void SplitConjuncts(ExprPtr e, std::vector<ExprPtr>& out) {
  if (e->kind == ExprKind::kAnd) {
    SplitConjuncts(std::move(e->children[0]), out);
    SplitConjuncts(std::move(e->children[1]), out);
  } else {
    out.push_back(std::move(e));
  }
}
ExprPtr RebuildConjunction(std::vector<ExprPtr> conj) {
  ExprPtr e = std::move(conj[0]);
  for (std::size_t i = 1; i < conj.size(); ++i) e = Expression::And(std::move(e), std::move(conj[i]));
  return e;
}

void PredicatePushdownImpl(PlanPtr& plan) {
  for (PlanPtr& c : plan->children) PredicatePushdownImpl(c);

  if (plan->op != LogicalOp::kFilter || plan->children[0]->op != LogicalOp::kJoin) return;

  PlanPtr join = std::move(plan->children[0]);
  const std::size_t lw = join->left_width;
  std::vector<ExprPtr> conj;
  SplitConjuncts(std::move(plan->predicate), conj);

  std::vector<ExprPtr> to_left, to_right, residual;
  for (ExprPtr& c : conj) {
    std::set<std::size_t> refs;
    CollectRefs(*c, refs);
    const bool all_left = std::all_of(refs.begin(), refs.end(), [&](std::size_t r) { return r < lw; });
    const bool all_right = std::all_of(refs.begin(), refs.end(), [&](std::size_t r) { return r >= lw; });
    if (!refs.empty() && all_left) {
      to_left.push_back(std::move(c));
    } else if (!refs.empty() && all_right) {
      RebaseRefs(*c, lw);  // right child columns are 0-based
      to_right.push_back(std::move(c));
    } else {
      residual.push_back(std::move(c));  // spans both sides (or constant): stays above the join
    }
  }

  auto wrap = [](PlanPtr child, std::vector<ExprPtr> preds) {
    PlanPtr f = MakeNode(LogicalOp::kFilter);
    f->predicate = RebuildConjunction(std::move(preds));
    f->out_types = child->out_types;
    f->out_names = child->out_names;
    f->children.push_back(std::move(child));
    return f;
  };
  if (!to_left.empty()) join->children[0] = wrap(std::move(join->children[0]), std::move(to_left));
  if (!to_right.empty()) join->children[1] = wrap(std::move(join->children[1]), std::move(to_right));

  if (residual.empty()) {
    plan = std::move(join);
  } else {
    plan->predicate = RebuildConjunction(std::move(residual));
    plan->children[0] = std::move(join);
  }
}

// ---- projection pushdown (single-table linear plans) ----

bool HasJoin(const LogicalNode& n) {
  if (n.op == LogicalOp::kJoin) return true;
  for (const PlanPtr& c : n.children) {
    if (HasJoin(*c)) return true;
  }
  return false;
}

}  // namespace

void ConstantFold(LogicalNode& plan) { FoldAllExprs(plan); }

void PredicatePushdown(PlanPtr& plan) { PredicatePushdownImpl(plan); }

void ProjectionPushdown(LogicalNode& plan) {
  if (HasJoin(plan)) return;  // multi-table pushdown deferred to P9

  // Descend the (single) child chain: remember the lowest schema-changer (Project
  // or Aggregate) and reach the Get.
  LogicalNode* sc = nullptr;
  LogicalNode* cur = &plan;
  while (cur->op != LogicalOp::kGet) {
    if (cur->op == LogicalOp::kProject || cur->op == LogicalOp::kAggregate) sc = cur;
    if (cur->children.empty()) return;
    cur = cur->children[0].get();
  }
  LogicalNode* get = cur;
  if (sc == nullptr) return;  // nothing references the table through a schema-changer

  // Collect table column refs from the schema-changer's inputs and any Filters
  // between it and the Get.
  std::set<std::size_t> needed;
  if (sc->op == LogicalOp::kProject) {
    for (const ExprPtr& p : sc->projections) CollectRefs(*p, needed);
  } else {
    for (const ExprPtr& g : sc->group_keys) CollectRefs(*g, needed);
    for (const AggregateItem& a : sc->aggregates) {
      if (a.arg) CollectRefs(*a.arg, needed);
    }
  }
  std::vector<LogicalNode*> filters;
  for (LogicalNode* m = sc->children[0].get(); m != get; m = m->children[0].get()) {
    if (m->op == LogicalOp::kFilter) {
      filters.push_back(m);
      CollectRefs(*m->predicate, needed);
    }
  }
  if (needed.size() == get->read_columns.size()) return;  // already minimal

  // Build read_columns + the original-index -> new-position remap.
  std::vector<std::size_t> read(needed.begin(), needed.end());  // sorted (std::set)
  std::vector<std::size_t> map(get->out_types.size(), 0);
  for (std::size_t pos = 0; pos < read.size(); ++pos) map[read[pos]] = pos;

  // Remap refs in the schema-changer inputs and the filters.
  if (sc->op == LogicalOp::kProject) {
    for (ExprPtr& p : sc->projections) RemapRefs(*p, map);
  } else {
    for (ExprPtr& g : sc->group_keys) RemapRefs(*g, map);
    for (AggregateItem& a : sc->aggregates) {
      if (a.arg) RemapRefs(*a.arg, map);
    }
  }
  for (LogicalNode* f : filters) RemapRefs(*f->predicate, map);

  // Prune the Get's output schema and that of the passthrough Filters.
  std::vector<TypeId> ptypes;
  std::vector<std::string> pnames;
  for (std::size_t c : read) {
    ptypes.push_back(get->out_types[c]);
    pnames.push_back(get->out_names[c]);
  }
  get->read_columns = read;
  get->out_types = ptypes;
  get->out_names = pnames;
  for (LogicalNode* f : filters) {
    f->out_types = ptypes;
    f->out_names = pnames;
  }
}

void Optimize(PlanPtr& plan) {
  ConstantFold(*plan);
  PredicatePushdown(plan);
  ProjectionPushdown(*plan);
}

}  // namespace strata
