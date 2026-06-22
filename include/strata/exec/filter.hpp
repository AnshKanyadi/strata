#pragma once

#include <cstddef>
#include <utility>

#include "strata/data/data_chunk.hpp"
#include "strata/data/selection_vector.hpp"
#include "strata/exec/expression.hpp"

namespace strata {

// Filter — applies a BOOL predicate to a chunk and produces a SELECTION VECTOR
// of the rows that pass. Only TRUE passes; both FALSE and NULL are filtered out
// (SQL three-valued WHERE). It does NOT copy or compact column data — it only
// narrows the live-row set (selection-vector discipline, ADR 0009).
class Filter {
 public:
  explicit Filter(ExprPtr predicate) : predicate_(std::move(predicate)) {}

  // Writes the passing row indices into out_sel (owned) and returns their count.
  std::size_t Select(const DataChunk& chunk, SelectionVector& out_sel);

 private:
  ExprPtr predicate_;
  ExpressionExecutor executor_;
};

}  // namespace strata
