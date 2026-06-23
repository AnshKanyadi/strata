#pragma once

#include <memory>

#include "strata/common/result.hpp"
#include "strata/exec/result_collector.hpp"
#include "strata/plan/catalog.hpp"
#include "strata/plan/logical_plan.hpp"

namespace strata {

// Lower a bound, optimized logical plan to physical operators and run it on the
// push pipeline, returning the materialized result. CONSUMES the plan's
// expressions (moves them into the operators). P7 supports single-table plans
// (Scan / Filter / Project / Aggregate / Order / Limit). See ADR 0014.
Result<std::unique_ptr<ResultCollector>> ExecutePlan(LogicalNode& plan, const Catalog& catalog);

}  // namespace strata
