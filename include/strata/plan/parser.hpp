#pragma once

#include <string_view>

#include "strata/common/result.hpp"
#include "strata/plan/logical_plan.hpp"

namespace strata {

// Parse one SELECT statement (Strata's supported subset) into an UNBOUND logical
// plan — column references carry names, not yet indices/types (the binder
// resolves those). A hand-written recursive-descent parser; see ADR 0014 for the
// subset and the parser-choice rationale. Errors surface via Result.
//
// Supported: SELECT (col | expr | agg) [AS name], ... FROM <table>
//   [WHERE <predicate>] [GROUP BY <expr>, ...] [ORDER BY <expr> [ASC|DESC]
//   [NULLS FIRST|LAST], ...] [LIMIT <n>]
// Aggregates: COUNT(*), COUNT(e), SUM(e), MIN(e), MAX(e), AVG(e).
// Expressions: column refs (t.col or col), int/double/string/DATE 'YYYY-MM-DD'
//   literals, + - *, comparisons, AND/OR/NOT, BETWEEN (desugars to >= AND <=),
//   parentheses.
Result<PlanPtr> ParseSql(std::string_view sql);

}  // namespace strata
