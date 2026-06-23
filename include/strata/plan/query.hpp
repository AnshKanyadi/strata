#pragma once

#include <memory>
#include <string_view>

#include "strata/common/result.hpp"
#include "strata/exec/result_collector.hpp"
#include "strata/plan/catalog.hpp"

namespace strata {

// The end-to-end SQL path: parse -> bind -> optimize -> execute, returning the
// materialized result. See ADR 0014.
Result<std::unique_ptr<ResultCollector>> Query(std::string_view sql, const Catalog& catalog);

}  // namespace strata
