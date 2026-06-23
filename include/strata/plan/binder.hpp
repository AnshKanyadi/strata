#pragma once

#include "strata/common/result.hpp"
#include "strata/plan/catalog.hpp"
#include "strata/plan/logical_plan.hpp"

namespace strata {

// Resolve an unbound plan against the catalog: bind column references to their
// (index, type) within each node's input schema, compute every node's output
// schema (out_types/out_names), coerce literal constants to the compared/operated
// type, and expand SELECT *. Errors (unknown table/column, type mismatch) return
// via Result. See ADR 0014.
Result<void> Bind(LogicalNode& plan, const Catalog& catalog);

}  // namespace strata
