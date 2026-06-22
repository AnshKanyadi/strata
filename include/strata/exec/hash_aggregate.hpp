#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "strata/data/data_chunk.hpp"
#include "strata/data/string_heap.hpp"
#include "strata/data/types.hpp"
#include "strata/exec/aggregate.hpp"
#include "strata/exec/pipeline.hpp"

namespace strata {

// HashAggregate — a Sink implementing GROUP BY (and global, no-key) aggregation.
// Open-addressing, linear-probing hash table with a salt and a row layout; see
// ADR 0010 (table) and ADR 0011 (aggregate state / NULL / overflow).
//
// Group rows live in one contiguous buffer; each row is:
//   [ uint64 hash | key values | key null-flags (1B each) | aggregate states ]
// A slots array maps a probe position to a group index + salt. On Finalize the
// groups are materialized into result chunks and pushed to a downstream Sink.
class HashAggregate final : public Sink {
 public:
  HashAggregate(std::vector<GroupKey> keys, std::vector<AggregateSpec> aggs, Sink& output);

  void Consume(const DataChunk& chunk) override;
  void Finalize() override;

  // Result schema: the group-key columns, then one column per aggregate.
  std::span<const TypeId> output_types() const noexcept { return output_types_; }
  std::size_t group_count() const noexcept { return num_groups_; }

 private:
  struct Slot {
    std::uint32_t index;  // group index, or kEmpty
    std::uint16_t salt;   // top bits of the hash, for fast rejection
  };
  static constexpr std::uint32_t kEmpty = 0xFFFFFFFFu;

  std::uint32_t FindOrCreateGroup(const DataChunk& chunk, std::size_t row, std::uint64_t hash);
  std::uint32_t AppendGroup(std::uint64_t hash);
  void Grow();
  std::uint64_t HashRow(const DataChunk& chunk, std::size_t row) const;
  bool KeysEqual(const std::byte* row, const DataChunk& chunk, std::size_t i) const;
  void WriteKeys(std::byte* row, const DataChunk& chunk, std::size_t i);
  void WriteKeyToOutput(Vector& out, std::size_t out_row, const std::byte* row,
                        std::size_t k) const;

  std::byte* RowPtr(std::uint32_t gi) { return rows_.data() + static_cast<std::size_t>(gi) * row_width_; }
  const std::byte* RowPtr(std::uint32_t gi) const {
    return rows_.data() + static_cast<std::size_t>(gi) * row_width_;
  }

  std::vector<GroupKey> keys_;
  std::vector<AggregateSpec> agg_specs_;
  std::vector<ResolvedAggregate> aggs_;
  std::vector<TypeId> output_types_;

  // Row layout (byte offsets within a group row).
  std::vector<std::size_t> key_offsets_;
  std::size_t null_offset_ = 0;
  std::vector<std::size_t> state_offsets_;
  std::size_t row_width_ = 0;

  std::vector<Slot> slots_;
  std::size_t mask_ = 0;  // slots_.size() - 1 (power of two)
  std::vector<std::byte> rows_;
  std::uint32_t num_groups_ = 0;
  StringHeap key_heap_;  // owns VARCHAR key bytes

  std::vector<std::uint32_t> group_idx_;  // reused scratch: group index per batch row
  Sink* output_;
};

}  // namespace strata
