#pragma once

#include <cstddef>
#include <cstdint>

#include "strata/data/types.hpp"
#include "strata/data/vector.hpp"

namespace strata {

enum class AggFunc : std::uint8_t { kCountStar, kCount, kSum, kMin, kMax, kAvg };

// One aggregate in a GROUP BY (or global aggregation).
struct AggregateSpec {
  AggFunc func;
  std::size_t input_col = 0;            // input chunk column (ignored for kCountStar)
  TypeId input_type = TypeId::kInt64;   // type of that column (ignored for kCountStar)
};

// A group key column.
struct GroupKey {
  std::size_t col;
  TypeId type;
};

// A (func, input-type) pair resolved to concrete operations over a group's state,
// which lives inline in the group row at `state_offset`. See ADR 0011. State is
// zero-initialized when a group is created (zero == the correct initial state:
// count 0, sum 0, has_value false), so there is no separate init step.
struct ResolvedAggregate {
  std::size_t state_size = 0;
  TypeId output_type = TypeId::kInt64;
  bool reads_input = true;  // false for COUNT(*)

  // Per-BATCH update (resolved once, loops internally — never per-row dispatch):
  // for each row i in [0,n) it updates the state of group `gidx[i]`. `col` is the
  // input column (null for COUNT(*)). NULLs are skipped except by COUNT(*).
  void (*update)(std::byte* rows, std::size_t row_width, std::size_t state_offset,
                 const std::uint32_t* gidx, const Vector* col, std::size_t n) = nullptr;

  // Produce the group's result value into out[out_row] (or mark it NULL).
  void (*finalize)(const std::byte* state, Vector& out, std::size_t out_row) = nullptr;

  // Merge a partial `src` state into `dst` (both states of this aggregate). Used
  // to combine per-worker thread-local tables in parallel aggregation (ADR 0015).
  // Merges RAW states (e.g. AVG's (sum,count)), so it must run before finalize.
  void (*combine)(std::byte* dst, const std::byte* src) = nullptr;
};

// Resolve an aggregate function + input type to its operations. Supported:
//   COUNT(*)            -> int64           (any input)
//   COUNT(col)          -> int64           (col of any type; counts non-NULLs)
//   SUM(int32|int64)    -> int64           (int64 accumulator; see overflow policy)
//   SUM(double)         -> double
//   MIN/MAX(int32|int64|double|date) -> same type
//   AVG(int32|int64|double) -> double
// Unsupported combinations return a ResolvedAggregate with null fn pointers; the
// HashAggregate constructor asserts they are non-null.
ResolvedAggregate ResolveAggregate(AggFunc func, TypeId input_type);

}  // namespace strata
