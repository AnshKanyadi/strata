#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "strata/data/data_chunk.hpp"
#include "strata/data/types.hpp"

namespace strata {

struct ColumnDef {
  std::string name;
  TypeId type;
};

// A table's column names + types. Caches a contiguous TypeId array so it can be
// handed to DataChunk::Initialize / exposed as a Source's output_types().
class Schema {
 public:
  Schema() = default;
  explicit Schema(std::vector<ColumnDef> columns) : columns_(std::move(columns)) {
    types_.reserve(columns_.size());
    for (const ColumnDef& c : columns_) types_.push_back(c.type);
  }

  std::size_t size() const noexcept { return columns_.size(); }
  const ColumnDef& column(std::size_t i) const { return columns_[i]; }
  const std::vector<ColumnDef>& columns() const noexcept { return columns_; }
  std::span<const TypeId> types() const noexcept { return types_; }

  std::optional<std::size_t> IndexOf(std::string_view name) const {
    for (std::size_t i = 0; i < columns_.size(); ++i) {
      if (columns_[i].name == name) return i;
    }
    return std::nullopt;
  }

 private:
  std::vector<ColumnDef> columns_;
  std::vector<TypeId> types_;
};

// In-memory columnar storage: a Schema plus a sequence of DataChunks, each
// holding up to kVectorSize rows (columnar within the chunk). Storing the table
// as execution-sized chunks makes Scan a zero-copy walk and pre-shapes the data
// for morsel-driven parallelism (a morsel = a contiguous run of chunks). See
// ADR 0007 for the row-group contrast and tradeoffs. Move-only (chunks are).
class ColumnarTable {
 public:
  explicit ColumnarTable(Schema schema) : schema_(std::move(schema)) {}

  ColumnarTable(ColumnarTable&&) = default;
  ColumnarTable& operator=(ColumnarTable&&) = default;
  ColumnarTable(const ColumnarTable&) = delete;
  ColumnarTable& operator=(const ColumnarTable&) = delete;

  const Schema& schema() const noexcept { return schema_; }
  std::span<const TypeId> column_types() const noexcept { return schema_.types(); }
  std::size_t row_count() const noexcept { return row_count_; }
  std::size_t chunk_count() const noexcept { return chunks_.size(); }
  const DataChunk& chunk(std::size_t i) const { return chunks_[i]; }

  // Builder entry point (used by the loader): take ownership of a populated chunk.
  void AppendChunk(DataChunk&& chunk) {
    row_count_ += chunk.size();
    chunks_.push_back(std::move(chunk));
  }

 private:
  Schema schema_;
  std::vector<DataChunk> chunks_;
  std::size_t row_count_ = 0;
};

}  // namespace strata
