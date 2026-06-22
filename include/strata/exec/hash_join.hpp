#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "strata/data/data_chunk.hpp"
#include "strata/data/selection_vector.hpp"
#include "strata/data/string_heap.hpp"
#include "strata/data/types.hpp"
#include "strata/exec/pipeline.hpp"

namespace strata {

// JoinHashTable — the build side of an equi-join (ADR 0012). CHAINED (not
// open-addressing like the aggregate table) because joins have duplicate keys:
// a slot holds the head build-row index of a chain, and each row stores a "next"
// index. Build rows are a ROW LAYOUT in one buffer:
//   [ uint64 hash | build column values | build null-flags (1B/col) | uint32 next ]
// so a probe match gathers one contiguous row. Build-side rows with any NULL
// join key are excluded (a NULL key never equi-matches), so stored keys are
// never NULL. The build side is fully materialized (deep-copied; strings into
// this table's heap) — it is a pipeline breaker.
class JoinHashTable {
 public:
  static constexpr std::uint32_t kEmpty = 0xFFFFFFFFu;

  JoinHashTable(std::span<const TypeId> build_types, std::vector<std::size_t> build_key_cols);

  void Insert(const DataChunk& build_chunk);  // build phase

  std::size_t row_count() const noexcept { return num_rows_; }
  std::size_t build_column_count() const noexcept { return build_types_.size(); }
  std::span<const TypeId> build_types() const noexcept { return build_types_; }

  // --- probe-side API (used by HashJoinProbe) ---
  bool AnyKeyNull(const DataChunk& chunk, const std::vector<std::size_t>& key_cols,
                  std::size_t i) const;
  std::uint64_t HashKeys(const DataChunk& chunk, const std::vector<std::size_t>& key_cols,
                         std::size_t i) const;
  std::uint32_t Head(std::uint64_t hash) const { return slots_[hash & mask_]; }
  std::uint32_t Next(std::uint32_t row) const;
  std::uint64_t RowHash(std::uint32_t row) const;
  bool RowKeyEquals(std::uint32_t build_row, const DataChunk& probe,
                    const std::vector<std::size_t>& probe_keys, std::size_t i) const;
  // Gather all build columns of the given matched rows into out columns
  // [out_col_base .. out_col_base + build_column_count()).
  void GatherBuild(const std::uint32_t* build_rows, std::size_t count, DataChunk& out,
                   std::size_t out_col_base) const;

 private:
  std::uint32_t AppendRow(std::uint64_t hash, const DataChunk& chunk, std::size_t i);
  void Grow();
  const std::byte* RowPtr(std::uint32_t r) const {
    return rows_.data() + static_cast<std::size_t>(r) * row_width_;
  }
  std::byte* RowPtr(std::uint32_t r) { return rows_.data() + static_cast<std::size_t>(r) * row_width_; }

  std::vector<TypeId> build_types_;
  std::vector<std::size_t> build_key_cols_;
  std::vector<TypeId> key_types_;  // join key types, in key order

  std::vector<std::size_t> col_offsets_;  // value offset of each build column in the row
  std::size_t null_offset_ = 0;
  std::size_t next_offset_ = 0;
  std::size_t row_width_ = 0;

  std::vector<std::uint32_t> slots_;  // head row index per slot (kEmpty if none)
  std::size_t mask_ = 0;
  std::vector<std::byte> rows_;
  std::uint32_t num_rows_ = 0;
  StringHeap heap_;
};

// HashJoinBuild — a Sink that builds a JoinHashTable from the build side.
class HashJoinBuild final : public Sink {
 public:
  HashJoinBuild(std::span<const TypeId> build_types, std::vector<std::size_t> build_key_cols)
      : table_(build_types, std::move(build_key_cols)) {}
  void Consume(const DataChunk& chunk) override { table_.Insert(chunk); }
  void Finalize() override {}
  const JoinHashTable& table() const noexcept { return table_; }

 private:
  JoinHashTable table_;
};

// HashJoinProbe — a Sink over probe chunks that emits (probe columns ++ build
// columns) for each inner-join match to a downstream Sink, gathering matches in
// batches of kVectorSize (handles fan-out).
class HashJoinProbe final : public Sink {
 public:
  HashJoinProbe(const JoinHashTable& table, std::span<const TypeId> probe_types,
                std::vector<std::size_t> probe_key_cols, Sink& output);
  void Consume(const DataChunk& probe_chunk) override;
  void Finalize() override { output_->Finalize(); }
  std::span<const TypeId> output_types() const noexcept { return output_types_; }

 private:
  void Flush(const DataChunk& probe_chunk, std::size_t count);

  const JoinHashTable* table_;
  std::vector<std::size_t> probe_key_cols_;
  std::size_t num_probe_cols_;
  std::vector<TypeId> output_types_;
  SelectionVector match_probe_;             // probe row index of each buffered match
  std::vector<std::uint32_t> match_build_;  // build row index of each buffered match
  Sink* output_;
};

}  // namespace strata
