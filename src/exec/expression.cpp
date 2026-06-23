#include "strata/exec/expression.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

#include "strata/data/column_ops.hpp"
#include "strata/data/string_ref.hpp"
#include "strata/data/types.hpp"
#include "strata/simd/kernels.hpp"
#include "strata/simd/scalar_kernels.hpp"

namespace strata {

// --- factories --------------------------------------------------------------

ExprPtr Expression::ColumnRef(std::size_t index, TypeId type) {
  auto e = std::make_unique<Expression>();
  e->kind = ExprKind::kColumnRef;
  e->type = type;
  e->column_index = index;
  return e;
}
ExprPtr Expression::ColumnRefByName(std::string name, std::string qualifier) {
  auto e = std::make_unique<Expression>();
  e->kind = ExprKind::kColumnRef;
  e->type = TypeId::kInt32;  // placeholder; the binder fills the real type + index
  e->column_name = std::move(name);
  e->table_qualifier = std::move(qualifier);
  return e;
}
ExprPtr Expression::Constant(Value v) {
  auto e = std::make_unique<Expression>();
  e->kind = ExprKind::kConstant;
  e->type = v.type;
  e->constant = std::move(v);
  return e;
}
ExprPtr Expression::Comparison(simd::CmpOp op, ExprPtr left, ExprPtr right) {
  auto e = std::make_unique<Expression>();
  e->kind = ExprKind::kComparison;
  e->type = TypeId::kBool;
  e->cmp = op;
  e->children.push_back(std::move(left));
  e->children.push_back(std::move(right));
  return e;
}
ExprPtr Expression::Arithmetic(simd::ArithOp op, ExprPtr left, ExprPtr right) {
  auto e = std::make_unique<Expression>();
  e->kind = ExprKind::kArithmetic;
  e->type = left->type;  // operands share a numeric type
  e->arith = op;
  e->children.push_back(std::move(left));
  e->children.push_back(std::move(right));
  return e;
}
ExprPtr Expression::And(ExprPtr left, ExprPtr right) {
  auto e = std::make_unique<Expression>();
  e->kind = ExprKind::kAnd;
  e->type = TypeId::kBool;
  e->children.push_back(std::move(left));
  e->children.push_back(std::move(right));
  return e;
}
ExprPtr Expression::Or(ExprPtr left, ExprPtr right) {
  auto e = std::make_unique<Expression>();
  e->kind = ExprKind::kOr;
  e->type = TypeId::kBool;
  e->children.push_back(std::move(left));
  e->children.push_back(std::move(right));
  return e;
}
ExprPtr Expression::Not(ExprPtr child) {
  auto e = std::make_unique<Expression>();
  e->kind = ExprKind::kNot;
  e->type = TypeId::kBool;
  e->children.push_back(std::move(child));
  return e;
}

namespace {

// An evaluated subexpression: either a freshly computed column we own, or a
// borrowed reference to an input column (the ColumnRef fast path — no copy).
struct ExprValue {
  std::optional<Vector> owned;
  const Vector* borrowed = nullptr;
  const Vector& get() const { return owned.has_value() ? *owned : *borrowed; }
};

// Strict-function NULL propagation: result is NULL wherever either operand is.
// This is ADR 0003's payoff; the all-valid fast path skips it entirely.
void PropagateNulls(Vector& out, const Vector& l, const Vector& r, std::size_t n) {
  if (l.validity().AllValid() && r.validity().AllValid()) return;  // fast path
  for (std::size_t i = 0; i < n; ++i) {
    if (!l.validity().RowIsValid(i) || !r.validity().RowIsValid(i)) out.validity().SetInvalid(i);
  }
}

std::uint8_t CompareStr(simd::CmpOp op, std::string_view a, std::string_view b) {
  bool r = false;
  switch (op) {
    case simd::CmpOp::kEq: r = (a == b); break;
    case simd::CmpOp::kNe: r = (a != b); break;
    case simd::CmpOp::kLt: r = (a < b);  break;
    case simd::CmpOp::kLe: r = (a <= b); break;
    case simd::CmpOp::kGt: r = (a > b);  break;
    case simd::CmpOp::kGe: r = (a >= b); break;
  }
  return r ? std::uint8_t{1} : std::uint8_t{0};
}

// Comparison values for all n rows (NULLs handled separately). Numeric types go
// through the SIMD kernels; bool/string are scalar (the SIMD ceiling, ADR 0008).
void EvalComparison(simd::CmpOp op, const Vector& l, const Vector& r, Vector& out, std::size_t n) {
  std::uint8_t* o = out.Data<std::uint8_t>();
  switch (l.type()) {
    case TypeId::kInt32:
    case TypeId::kDate:
      simd::Compare(op, l.Data<std::int32_t>(), r.Data<std::int32_t>(), o, n);
      break;
    case TypeId::kInt64:
      simd::Compare(op, l.Data<std::int64_t>(), r.Data<std::int64_t>(), o, n);
      break;
    case TypeId::kDouble:
      simd::Compare(op, l.Data<double>(), r.Data<double>(), o, n);
      break;
    case TypeId::kBool: {
      const std::uint8_t* a = l.Data<std::uint8_t>();
      const std::uint8_t* b = r.Data<std::uint8_t>();
      for (std::size_t i = 0; i < n; ++i) o[i] = simd::scalar::ApplyCmp(op, a[i], b[i]);
      break;
    }
    case TypeId::kVarchar: {
      const StringRef* a = l.Data<StringRef>();
      const StringRef* b = r.Data<StringRef>();
      for (std::size_t i = 0; i < n; ++i) o[i] = CompareStr(op, a[i].view(), b[i].view());
      break;
    }
  }
}

void EvalArith(simd::ArithOp op, const Vector& l, const Vector& r, Vector& out, std::size_t n) {
  switch (l.type()) {
    case TypeId::kInt32:
    case TypeId::kDate:
      simd::Arith(op, l.Data<std::int32_t>(), r.Data<std::int32_t>(), out.Data<std::int32_t>(), n);
      break;
    case TypeId::kInt64:
      simd::Arith(op, l.Data<std::int64_t>(), r.Data<std::int64_t>(), out.Data<std::int64_t>(), n);
      break;
    case TypeId::kDouble:
      simd::Arith(op, l.Data<double>(), r.Data<double>(), out.Data<double>(), n);
      break;
    default:
      break;  // arithmetic is only defined on numeric types
  }
}

// Three-valued AND (NOT a simple mask-AND): FALSE dominates even over NULL.
void EvalAnd(const Vector& l, const Vector& r, Vector& out, std::size_t n) {
  const std::uint8_t* lv = l.Data<std::uint8_t>();
  const std::uint8_t* rv = r.Data<std::uint8_t>();
  std::uint8_t* o = out.Data<std::uint8_t>();
  for (std::size_t i = 0; i < n; ++i) {
    const bool l_false = l.validity().RowIsValid(i) && lv[i] == 0;
    const bool r_false = r.validity().RowIsValid(i) && rv[i] == 0;
    if (l_false || r_false) {
      o[i] = 0;  // FALSE AND anything == FALSE (even FALSE AND NULL)
    } else if (l.validity().RowIsValid(i) && r.validity().RowIsValid(i)) {
      o[i] = 1;  // both TRUE
    } else {
      out.validity().SetInvalid(i);  // NULL
    }
  }
}

// Three-valued OR: TRUE dominates even over NULL.
void EvalOr(const Vector& l, const Vector& r, Vector& out, std::size_t n) {
  const std::uint8_t* lv = l.Data<std::uint8_t>();
  const std::uint8_t* rv = r.Data<std::uint8_t>();
  std::uint8_t* o = out.Data<std::uint8_t>();
  for (std::size_t i = 0; i < n; ++i) {
    const bool l_true = l.validity().RowIsValid(i) && lv[i] != 0;
    const bool r_true = r.validity().RowIsValid(i) && rv[i] != 0;
    if (l_true || r_true) {
      o[i] = 1;  // TRUE OR anything == TRUE (even TRUE OR NULL)
    } else if (l.validity().RowIsValid(i) && r.validity().RowIsValid(i)) {
      o[i] = 0;  // both FALSE
    } else {
      out.validity().SetInvalid(i);  // NULL
    }
  }
}

// NOT NULL == NULL; otherwise flip.
void EvalNot(const Vector& c, Vector& out, std::size_t n) {
  const std::uint8_t* cv = c.Data<std::uint8_t>();
  std::uint8_t* o = out.Data<std::uint8_t>();
  for (std::size_t i = 0; i < n; ++i) {
    if (c.validity().RowIsValid(i)) {
      o[i] = cv[i] == 0 ? 1 : 0;
    } else {
      out.validity().SetInvalid(i);
    }
  }
}

// Broadcast a constant to a flat column of n rows. A deliberate simplification
// (constant-vector fast paths are deferred — ADR 0009); keeps kernel operands
// uniform flat arrays.
Vector BroadcastConstant(const Value& v, std::size_t n, std::size_t cap) {
  Vector out(v.type, cap);
  if (v.is_null) {
    for (std::size_t i = 0; i < n; ++i) out.SetNull(i);
    return out;
  }
  switch (v.type) {
    case TypeId::kBool:
      for (std::size_t i = 0; i < n; ++i) out.Set<std::uint8_t>(i, static_cast<std::uint8_t>(v.i));
      break;
    case TypeId::kInt32:
    case TypeId::kDate:
      for (std::size_t i = 0; i < n; ++i) out.Set<std::int32_t>(i, static_cast<std::int32_t>(v.i));
      break;
    case TypeId::kInt64:
      for (std::size_t i = 0; i < n; ++i) out.Set<std::int64_t>(i, v.i);
      break;
    case TypeId::kDouble:
      for (std::size_t i = 0; i < n; ++i) out.Set<double>(i, v.d);
      break;
    case TypeId::kVarchar:
      for (std::size_t i = 0; i < n; ++i) out.Set<StringRef>(i, out.AddString(v.s));
      break;
  }
  return out;
}

ExprValue Eval(const Expression& e, const DataChunk& chunk) {
  const std::size_t n = chunk.size();
  const std::size_t cap = chunk.capacity();
  switch (e.kind) {
    case ExprKind::kColumnRef:
      return ExprValue{std::nullopt, &chunk.column(e.column_index)};
    case ExprKind::kConstant:
      return ExprValue{BroadcastConstant(e.constant, n, cap), nullptr};
    case ExprKind::kComparison: {
      const ExprValue l = Eval(*e.children[0], chunk);
      const ExprValue r = Eval(*e.children[1], chunk);
      Vector out(TypeId::kBool, cap);
      EvalComparison(e.cmp, l.get(), r.get(), out, n);
      PropagateNulls(out, l.get(), r.get(), n);
      return ExprValue{std::move(out), nullptr};
    }
    case ExprKind::kArithmetic: {
      const ExprValue l = Eval(*e.children[0], chunk);
      const ExprValue r = Eval(*e.children[1], chunk);
      Vector out(e.type, cap);
      EvalArith(e.arith, l.get(), r.get(), out, n);
      PropagateNulls(out, l.get(), r.get(), n);
      return ExprValue{std::move(out), nullptr};
    }
    case ExprKind::kAnd: {
      const ExprValue l = Eval(*e.children[0], chunk);
      const ExprValue r = Eval(*e.children[1], chunk);
      Vector out(TypeId::kBool, cap);
      EvalAnd(l.get(), r.get(), out, n);
      return ExprValue{std::move(out), nullptr};
    }
    case ExprKind::kOr: {
      const ExprValue l = Eval(*e.children[0], chunk);
      const ExprValue r = Eval(*e.children[1], chunk);
      Vector out(TypeId::kBool, cap);
      EvalOr(l.get(), r.get(), out, n);
      return ExprValue{std::move(out), nullptr};
    }
    case ExprKind::kNot: {
      const ExprValue c = Eval(*e.children[0], chunk);
      Vector out(TypeId::kBool, cap);
      EvalNot(c.get(), out, n);
      return ExprValue{std::move(out), nullptr};
    }
  }
  return ExprValue{Vector(TypeId::kBool, cap), nullptr};  // unreachable
}

}  // namespace

Vector ExpressionExecutor::Execute(const Expression& expr, const DataChunk& chunk) {
  ExprValue v = Eval(expr, chunk);
  if (v.owned.has_value()) return std::move(*v.owned);
  // Bare ColumnRef result: materialize an owned copy (e.g. SELECT bare_column).
  Vector out(v.borrowed->type(), chunk.capacity());
  CopyColumn(*v.borrowed, out, chunk.size());
  return out;
}

}  // namespace strata
