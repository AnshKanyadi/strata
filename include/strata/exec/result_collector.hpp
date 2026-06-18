#pragma once

#include <cstddef>
#include <ostream>
#include <string>
#include <utility>
#include <vector>

#include "strata/data/data_chunk.hpp"
#include "strata/exec/pipeline.hpp"
#include "strata/storage/columnar_table.hpp"

namespace strata {

// ResultCollector — a Sink that materializes the pushed batches into its own
// storage so the result outlives the source. Because Source batches are only
// borrowed (ADR 0006), Consume DEEP-COPIES each chunk, including VARCHAR bytes
// into the collector's own per-column StringHeap.
class ResultCollector final : public Sink {
 public:
  explicit ResultCollector(Schema schema) : schema_(std::move(schema)) {}

  void Consume(const DataChunk& chunk) override;

  std::size_t row_count() const noexcept { return row_count_; }
  std::size_t column_count() const noexcept { return schema_.size(); }
  const Schema& schema() const noexcept { return schema_; }
  const std::vector<DataChunk>& chunks() const noexcept { return chunks_; }

  // Flattened, by-(row,col) accessors that walk the stored chunks (test/print
  // convenience — O(chunks) per call, not a hot path).
  bool IsNull(std::size_t row, std::size_t col) const;
  template <class T>
  T Get(std::size_t row, std::size_t col) const {
    const auto [ci, off] = Locate(row);
    return chunks_[ci].column(col).Get<T>(off);  // column() is a concrete Vector&
  }
  std::string GetString(std::size_t row, std::size_t col) const;

  // Pretty-print the whole result set (header + rows) — the "result-printing sink".
  void Print(std::ostream& os) const;

 private:
  std::pair<std::size_t, std::size_t> Locate(std::size_t row) const;  // -> (chunk, offset)

  Schema schema_;
  std::vector<DataChunk> chunks_;
  std::size_t row_count_ = 0;
};

}  // namespace strata
