#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "strata/data/data_chunk.hpp"
#include "strata/data/types.hpp"
#include "strata/data/vector.hpp"
#include "strata/simd/kernels.hpp"  // ArithOp, CmpOp

namespace strata {

// A typed literal constant operand.
struct Value {
  TypeId type = TypeId::kInt32;
  bool is_null = false;
  std::int64_t i = 0;  // INT32 / INT64 / DATE / BOOL
  double d = 0.0;      // DOUBLE
  std::string s;       // VARCHAR

  static Value Int32(std::int32_t v) { return {TypeId::kInt32, false, v, 0.0, {}}; }
  static Value Int64(std::int64_t v) { return {TypeId::kInt64, false, v, 0.0, {}}; }
  static Value Double(double v) { return {TypeId::kDouble, false, 0, v, {}}; }
  static Value Bool(bool v) { return {TypeId::kBool, false, v ? 1 : 0, 0.0, {}}; }
  static Value Date(std::int32_t days) { return {TypeId::kDate, false, days, 0.0, {}}; }
  static Value Varchar(std::string v) { return {TypeId::kVarchar, false, 0, 0.0, std::move(v)}; }
  static Value Null(TypeId t) { return {t, true, 0, 0.0, {}}; }
};

enum class ExprKind : std::uint8_t {
  kColumnRef,
  kConstant,
  kComparison,
  kArithmetic,
  kAnd,
  kOr,
  kNot,
};

class Expression;
using ExprPtr = std::unique_ptr<Expression>;

// A node in the expression tree (a tagged struct rather than a polymorphic
// hierarchy — simpler to build, walk, and reason about). See ADR 0009.
class Expression {
 public:
  ExprKind kind;
  TypeId type;  // result type (computed by the factory)

  std::size_t column_index = 0;                  // kColumnRef (after binding)
  std::string column_name;                       // kColumnRef (before binding; set by the parser)
  std::string table_qualifier;                   // kColumnRef (optional "t" in t.col)
  Value constant{};                              // kConstant
  simd::CmpOp cmp = simd::CmpOp::kEq;             // kComparison
  simd::ArithOp arith = simd::ArithOp::kAdd;      // kArithmetic
  std::vector<ExprPtr> children;                 // operands

  static ExprPtr ColumnRef(std::size_t index, TypeId type);
  // Unbound column reference (name only); the binder fills column_index + type.
  static ExprPtr ColumnRefByName(std::string name, std::string qualifier = {});
  static ExprPtr Constant(Value v);
  static ExprPtr Comparison(simd::CmpOp op, ExprPtr left, ExprPtr right);   // -> kBool
  static ExprPtr Arithmetic(simd::ArithOp op, ExprPtr left, ExprPtr right); // -> operand type
  static ExprPtr And(ExprPtr left, ExprPtr right);                          // -> kBool
  static ExprPtr Or(ExprPtr left, ExprPtr right);                           // -> kBool
  static ExprPtr Not(ExprPtr child);                                        // -> kBool
};

// Evaluates an Expression over a DataChunk, producing the result column.
// NULL-aware three-valued logic; dispatches once per batch to the SIMD kernels
// for numeric comparison/arithmetic and to scalar loops for logical/string ops.
// Stateless for now (a result-vector pool is a deferred optimization). ADR 0009.
class ExpressionExecutor {
 public:
  // Evaluate over chunk.size() rows; result Vector has chunk.capacity() capacity.
  Vector Execute(const Expression& expr, const DataChunk& chunk);
};

}  // namespace strata
