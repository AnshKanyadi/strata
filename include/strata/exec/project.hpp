#pragma once

#include <cstddef>
#include <span>
#include <vector>

#include "strata/data/data_chunk.hpp"
#include "strata/data/selection_vector.hpp"
#include "strata/data/types.hpp"
#include "strata/exec/expression.hpp"

namespace strata {

// Project — evaluates one expression per output column and GATHERS the selected
// rows into a dense output chunk. Projection is where materialization happens in
// the selection-vector model (ADR 0009): Filter narrows, Project compacts.
class Project {
 public:
  explicit Project(std::vector<ExprPtr> exprs);

  std::span<const TypeId> output_types() const noexcept { return out_types_; }
  std::size_t column_count() const noexcept { return exprs_.size(); }

  // output.column(c)[j] = expr_c(input[sel.Get(j)]) for j in [0, count). Sets
  // output's size to count. `output` must be Initialized with output_types().
  void Execute(const DataChunk& input, const SelectionVector& sel, std::size_t count,
               DataChunk& output);

 private:
  std::vector<ExprPtr> exprs_;
  std::vector<TypeId> out_types_;
  ExpressionExecutor executor_;
};

}  // namespace strata
