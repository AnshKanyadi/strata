#include "strata/plan/binder.hpp"

#include <charconv>
#include <cstdint>
#include <string>
#include <vector>

#include "strata/common/error.hpp"
#include "strata/exec/aggregate.hpp"
#include "strata/exec/expression.hpp"
#include "strata/util/date.hpp"

namespace strata {
namespace {

struct BindFail {
  std::string msg;
};
[[noreturn]] void Fail(std::string msg) { throw BindFail{std::move(msg)}; }

bool IsNumeric(TypeId t) {
  return t == TypeId::kInt32 || t == TypeId::kInt64 || t == TypeId::kDouble || t == TypeId::kDate;
}

// Coerce a literal constant node to `target` (numeric widening / string->date).
void CoerceConstant(Expression& c, TypeId target) {
  Value& v = c.constant;
  if (v.type == target) return;
  if (IsNumeric(target) && IsNumeric(v.type)) {
    const double dval = (v.type == TypeId::kDouble) ? v.d : static_cast<double>(v.i);
    const std::int64_t ival = (v.type == TypeId::kDouble) ? static_cast<std::int64_t>(v.d) : v.i;
    if (target == TypeId::kDouble) {
      v.d = dval;
    } else {
      v.i = ival;  // int32/int64/date all stored in v.i
    }
  } else if (target == TypeId::kDate && v.type == TypeId::kVarchar) {
    // parse 'YYYY-MM-DD'
    const std::string& s = v.s;
    const auto d1 = s.find('-');
    const auto d2 = (d1 == std::string::npos) ? d1 : s.find('-', d1 + 1);
    if (d1 == std::string::npos || d2 == std::string::npos) Fail("cannot parse date literal: " + s);
    auto num = [&](std::string_view sv) {
      int x = 0;
      const auto [p, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), x);
      if (ec != std::errc{} || p != sv.data() + sv.size()) Fail("cannot parse date literal: " + s);
      return x;
    };
    const std::string_view sv = s;
    v.i = DaysFromCivil(num(sv.substr(0, d1)), num(sv.substr(d1 + 1, d2 - d1 - 1)),
                        num(sv.substr(d2 + 1)));
  } else {
    Fail("cannot coerce a constant from a non-matching type");
  }
  v.type = target;
  c.type = target;
}

// For a binary node, if the two operand types differ, coerce a constant operand
// to the other's type (so the kernels see one type). Two differing columns are
// an error in P7 (no implicit column casts).
void Unify(Expression& a, Expression& b) {
  if (a.type == b.type) return;
  if (b.kind == ExprKind::kConstant) {
    CoerceConstant(b, a.type);
  } else if (a.kind == ExprKind::kConstant) {
    CoerceConstant(a, b.type);
  } else {
    Fail("type mismatch between two non-constant operands");
  }
}

TypeId BindExpr(Expression& e, const std::vector<std::string>& names,
                const std::vector<TypeId>& types) {
  switch (e.kind) {
    case ExprKind::kColumnRef: {
      for (std::size_t i = 0; i < names.size(); ++i) {
        if (names[i] == e.column_name) {
          e.column_index = i;
          e.type = types[i];
          return e.type;
        }
      }
      Fail("unknown column: " + e.column_name);
    }
    case ExprKind::kConstant:
      return e.type;
    case ExprKind::kComparison: {
      BindExpr(*e.children[0], names, types);
      BindExpr(*e.children[1], names, types);
      Unify(*e.children[0], *e.children[1]);
      e.type = TypeId::kBool;
      return e.type;
    }
    case ExprKind::kArithmetic: {
      BindExpr(*e.children[0], names, types);
      BindExpr(*e.children[1], names, types);
      Unify(*e.children[0], *e.children[1]);
      e.type = e.children[0]->type;
      return e.type;
    }
    case ExprKind::kAnd:
    case ExprKind::kOr:
      BindExpr(*e.children[0], names, types);
      BindExpr(*e.children[1], names, types);
      e.type = TypeId::kBool;
      return e.type;
    case ExprKind::kNot:
      BindExpr(*e.children[0], names, types);
      e.type = TypeId::kBool;
      return e.type;
  }
  return e.type;
}

std::string DeriveName(const Expression& e, const std::vector<std::string>& child_names,
                       std::size_t fallback_idx) {
  if (e.kind == ExprKind::kColumnRef && e.column_index < child_names.size()) {
    return child_names[e.column_index];
  }
  return "expr" + std::to_string(fallback_idx);
}

const char* AggName(AggFunc f) {
  switch (f) {
    case AggFunc::kCountStar: return "count_star";
    case AggFunc::kCount:     return "count";
    case AggFunc::kSum:       return "sum";
    case AggFunc::kMin:       return "min";
    case AggFunc::kMax:       return "max";
    case AggFunc::kAvg:       return "avg";
  }
  return "agg";
}

void BindNode(LogicalNode& n, const Catalog& cat) {
  for (PlanPtr& ch : n.children) BindNode(*ch, cat);

  switch (n.op) {
    case LogicalOp::kGet: {
      const ColumnarTable* t = cat.Find(n.table);
      if (t == nullptr) Fail("unknown table: " + n.table);
      const Schema& s = t->schema();
      for (std::size_t i = 0; i < s.size(); ++i) {
        n.out_names.push_back(s.column(i).name);
        n.out_types.push_back(s.column(i).type);
        n.read_columns.push_back(i);  // all columns; projection pushdown prunes later
      }
      break;
    }
    case LogicalOp::kFilter: {
      const LogicalNode& ch = *n.children[0];
      BindExpr(*n.predicate, ch.out_names, ch.out_types);
      n.out_names = ch.out_names;
      n.out_types = ch.out_types;
      break;
    }
    case LogicalOp::kProject: {
      const LogicalNode& ch = *n.children[0];
      if (n.project_all) {
        n.projections.clear();
        n.proj_names.clear();
        for (std::size_t i = 0; i < ch.out_types.size(); ++i) {
          n.projections.push_back(Expression::ColumnRef(i, ch.out_types[i]));
          n.proj_names.push_back(ch.out_names[i]);
        }
      } else {
        for (std::size_t k = 0; k < n.projections.size(); ++k) {
          BindExpr(*n.projections[k], ch.out_names, ch.out_types);
          if (n.proj_names[k].empty()) n.proj_names[k] = DeriveName(*n.projections[k], ch.out_names, k);
        }
      }
      for (std::size_t k = 0; k < n.projections.size(); ++k) {
        n.out_types.push_back(n.projections[k]->type);
        n.out_names.push_back(n.proj_names[k]);
      }
      break;
    }
    case LogicalOp::kAggregate: {
      const LogicalNode& ch = *n.children[0];
      for (ExprPtr& gk : n.group_keys) BindExpr(*gk, ch.out_names, ch.out_types);
      for (AggregateItem& a : n.aggregates) {
        if (a.arg) BindExpr(*a.arg, ch.out_names, ch.out_types);
      }
      for (std::size_t k = 0; k < n.group_keys.size(); ++k) {
        n.out_types.push_back(n.group_keys[k]->type);
        n.out_names.push_back(DeriveName(*n.group_keys[k], ch.out_names, k));
      }
      for (AggregateItem& a : n.aggregates) {
        const TypeId argt = a.arg ? a.arg->type : TypeId::kInt32;
        const ResolvedAggregate r = ResolveAggregate(a.func, argt);
        if (r.update == nullptr) Fail("unsupported aggregate for the argument type");
        n.out_types.push_back(r.output_type);
        n.out_names.push_back(a.alias.empty() ? AggName(a.func) : a.alias);
      }
      break;
    }
    case LogicalOp::kJoin: {
      const LogicalNode& l = *n.children[0];
      const LogicalNode& r = *n.children[1];
      for (ExprPtr& k : n.left_keys) BindExpr(*k, l.out_names, l.out_types);
      for (ExprPtr& k : n.right_keys) BindExpr(*k, r.out_names, r.out_types);
      n.left_width = l.out_types.size();
      n.out_names = l.out_names;
      n.out_types = l.out_types;
      n.out_names.insert(n.out_names.end(), r.out_names.begin(), r.out_names.end());
      n.out_types.insert(n.out_types.end(), r.out_types.begin(), r.out_types.end());
      break;
    }
    case LogicalOp::kOrder: {
      const LogicalNode& ch = *n.children[0];
      for (OrderItem& o : n.order) BindExpr(*o.expr, ch.out_names, ch.out_types);
      n.out_names = ch.out_names;
      n.out_types = ch.out_types;
      break;
    }
    case LogicalOp::kLimit: {
      const LogicalNode& ch = *n.children[0];
      n.out_names = ch.out_names;
      n.out_types = ch.out_types;
      break;
    }
  }
}

}  // namespace

Result<void> Bind(LogicalNode& plan, const Catalog& catalog) {
  try {
    BindNode(plan, catalog);
    return Ok();
  } catch (const BindFail& e) {
    return Err(ErrorCode::kParseError, e.msg);
  }
}

}  // namespace strata
