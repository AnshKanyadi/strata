#pragma once

#include <vector>

#include "strata/exec/aggregate.hpp"
#include "strata/exec/pipeline.hpp"
#include "strata/parallel/thread_pool.hpp"
#include "strata/storage/columnar_table.hpp"

namespace strata {

// Morsel-driven parallel GROUP BY / global aggregation over `table` (ADR 0015).
// Each worker accumulates morsels (chunk ranges) into a THREAD-LOCAL hash table;
// after the barrier the per-worker partials are merged (state-level combine) and
// finalized into `output`. `keys`/`specs` reference table column indices. The
// result equals the serial HashAggregate (bit-identical for integer aggregates;
// see ADR 0015 on FP non-associativity).
void ParallelAggregate(ThreadPool& pool, const ColumnarTable& table,
                       std::vector<GroupKey> keys, std::vector<AggregateSpec> specs,
                       Sink& output);

}  // namespace strata
