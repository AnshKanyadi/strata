#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "strata/data/data_chunk.hpp"
#include "strata/data/types.hpp"
#include "strata/exec/pipeline.hpp"
#include "strata/exec/sort.hpp"

namespace strata {

// TopN — ORDER BY ... LIMIT k via a BOUNDED HEAP of size k, instead of a full
// sort. A max-heap keyed by the sort order whose ROOT is the WORST kept row; a
// new row that sorts before the root replaces it. O(N log k) time, O(k) memory.
// Uses the same comparator as Sort, so its output equals full-sort-then-LIMIT-k.
// See ADR 0013.
class TopN final : public Sink {
 public:
  TopN(std::vector<SortKey> keys, std::size_t k, Sink& output);
  void Consume(const DataChunk& chunk) override;
  void Finalize() override;

 private:
  std::vector<SortKey> keys_;
  std::size_t k_;
  std::vector<TypeId> types_;          // input schema (lazy)
  DataChunk kept_;                     // capacity k_; holds up to k_ candidate rows
  std::size_t count_ = 0;              // rows currently kept
  std::vector<std::uint32_t> heap_;    // slot indices into kept_, max-heap (root = worst)
  Sink* output_;
};

}  // namespace strata
