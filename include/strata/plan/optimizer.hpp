#pragma once

#include "strata/plan/logical_plan.hpp"

namespace strata {

// Rule-based (heuristic) optimizer over a BOUND plan, in place. See ADR 0014.
// Order: constant folding, predicate pushdown, projection pushdown.
void Optimize(PlanPtr& plan);

// Individually exposed for plan-rewrite assertion tests:
void ConstantFold(LogicalNode& plan);            // fold constant sub-expressions
void PredicatePushdown(PlanPtr& plan);           // split conjuncts; push below joins/projects
void ProjectionPushdown(LogicalNode& plan);      // prune a single Get to referenced columns

}  // namespace strata
