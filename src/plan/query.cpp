#include "strata/plan/query.hpp"

#include <utility>

#include "strata/plan/binder.hpp"
#include "strata/plan/executor.hpp"
#include "strata/plan/optimizer.hpp"
#include "strata/plan/parser.hpp"

namespace strata {

Result<std::unique_ptr<ResultCollector>> Query(std::string_view sql, const Catalog& catalog) {
  Result<PlanPtr> parsed = ParseSql(sql);
  if (!parsed) return std::unexpected(std::move(parsed).error());
  PlanPtr plan = std::move(*parsed);

  Status bound = Bind(*plan, catalog);
  if (!bound) return std::unexpected(std::move(bound).error());

  Optimize(plan);
  return ExecutePlan(*plan, catalog);
}

}  // namespace strata
