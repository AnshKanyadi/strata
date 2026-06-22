#pragma once

#include <cstddef>
#include <vector>

#include "strata/data/data_chunk.hpp"
#include "strata/data/types.hpp"
#include "strata/exec/pipeline.hpp"

namespace strata {

// One ORDER BY key. NULL placement is absolute (nulls_first), independent of
// ASC/DESC — matching DuckDB, whose default is NULLS LAST for both directions
// (verified). See ADR 0013.
struct SortKey {
  std::size_t col;
  TypeId type;
  bool ascending = true;
  bool nulls_first = false;  // DuckDB default: NULLS LAST (both ASC and DESC)
};

// Total order over rows per `keys`: <0 if (a@ra) sorts before (b@rb), 0 if equal
// on all keys, >0 otherwise. Shared by Sort and TopN so their orderings agree.
int CompareRows(const DataChunk& a, std::size_t ra, const DataChunk& b, std::size_t rb,
                const std::vector<SortKey>& keys);

// Sort — a pipeline-breaker Sink. Materializes all input (repacked into its own
// kVectorSize chunks for O(1) row locate), std::stable_sort's an index array by
// `keys` (sorting indices, not moving wide rows), then gathers rows in order to
// the downstream sink. Stable: equal-key rows keep input order.
class Sort final : public Sink {
 public:
  Sort(std::vector<SortKey> keys, Sink& output);
  void Consume(const DataChunk& chunk) override;
  void Finalize() override;

 private:
  void Append(const DataChunk& src, std::size_t i);  // copy one input row into storage

  std::vector<SortKey> keys_;
  std::vector<TypeId> types_;       // input schema (lazy-initialized)
  std::vector<DataChunk> chunks_;   // materialized; each kVectorSize except the last
  std::size_t total_ = 0;
  Sink* output_;
};

}  // namespace strata
