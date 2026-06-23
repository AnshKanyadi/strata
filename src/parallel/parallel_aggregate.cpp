#include "strata/parallel/parallel_aggregate.hpp"

#include <cstddef>
#include <memory>
#include <utility>

#include "strata/exec/hash_aggregate.hpp"

namespace strata {
namespace {

// A Sink that discards everything — given to the thread-local accumulators,
// which only accumulate (they are merged, never finalized to a real output).
struct NullSink final : Sink {
  void Consume(const DataChunk&) override {}
  void Finalize() override {}
};

}  // namespace

void ParallelAggregate(ThreadPool& pool, const ColumnarTable& table, std::vector<GroupKey> keys,
                       std::vector<AggregateSpec> specs, Sink& output) {
  const std::size_t nthreads = pool.size();
  const std::size_t nchunks = table.chunk_count();

  NullSink null_sink;  // outlives `locals` (declared first; destroyed last)

  // One thread-local accumulator per worker.
  std::vector<std::unique_ptr<HashAggregate>> locals;
  locals.reserve(nthreads);
  for (std::size_t w = 0; w < nthreads; ++w) {
    locals.push_back(std::make_unique<HashAggregate>(keys, specs, null_sink));  // copies keys/specs
  }

  // Morsel = one table chunk. Worker `wid` folds chunk `m` into its own table.
  // locals[wid] is touched only by the worker running the task -> no data race.
  pool.ParallelFor(nchunks,
                   [&](std::size_t m, std::size_t wid) { locals[wid]->Consume(table.chunk(m)); });

  // Single-threaded merge (after the ParallelFor barrier) -> final -> output.
  HashAggregate final_agg(std::move(keys), std::move(specs), output);
  for (std::size_t w = 0; w < nthreads; ++w) final_agg.MergeFrom(*locals[w]);
  final_agg.Finalize();
}

}  // namespace strata
