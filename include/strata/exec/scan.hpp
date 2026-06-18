#pragma once

#include <cstddef>
#include <span>

#include "strata/data/types.hpp"
#include "strata/exec/pipeline.hpp"
#include "strata/storage/columnar_table.hpp"

namespace strata {

// Scan — a Source that walks a ColumnarTable's stored chunks and hands each back
// by const pointer (ZERO COPY; this is the reason GetChunk borrows rather than
// fills a caller buffer — see ADR 0006). It does not own the table.
class Scan final : public Source {
 public:
  explicit Scan(const ColumnarTable& table) : table_(&table) {}

  const DataChunk* GetChunk() override {
    if (next_ >= table_->chunk_count()) return nullptr;
    return &table_->chunk(next_++);
  }

  std::span<const TypeId> output_types() const override { return table_->column_types(); }

 private:
  const ColumnarTable* table_;
  std::size_t next_ = 0;
};

}  // namespace strata
