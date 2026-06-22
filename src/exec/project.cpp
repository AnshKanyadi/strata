#include "strata/exec/project.hpp"

#include <utility>

#include "strata/data/column_ops.hpp"
#include "strata/data/vector.hpp"

namespace strata {

Project::Project(std::vector<ExprPtr> exprs) : exprs_(std::move(exprs)) {
  out_types_.reserve(exprs_.size());
  for (const ExprPtr& e : exprs_) out_types_.push_back(e->type);
}

void Project::Execute(const DataChunk& input, const SelectionVector& sel, std::size_t count,
                      DataChunk& output) {
  for (std::size_t c = 0; c < exprs_.size(); ++c) {
    // Evaluate over the full chunk, then gather only the selected rows. (Pushing
    // the selection into evaluation is a deferred optimization — ADR 0009.)
    const Vector result = executor_.Execute(*exprs_[c], input);
    GatherColumn(result, sel, count, output.column(c));
  }
  output.SetSize(count);
}

}  // namespace strata
