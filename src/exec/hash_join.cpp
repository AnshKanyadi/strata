#include "strata/exec/hash_join.hpp"

#include <cstring>
#include <utility>

#include "strata/data/column_ops.hpp"
#include "strata/data/string_ref.hpp"
#include "strata/data/vector.hpp"
#include "strata/exec/hash_util.hpp"

namespace strata {
namespace {
constexpr std::size_t Align8(std::size_t x) { return (x + 7) & ~std::size_t{7}; }
}  // namespace

// ============================ JoinHashTable ================================

JoinHashTable::JoinHashTable(std::span<const TypeId> build_types,
                             std::vector<std::size_t> build_key_cols)
    : build_types_(build_types.begin(), build_types.end()),
      build_key_cols_(std::move(build_key_cols)) {
  key_types_.reserve(build_key_cols_.size());
  for (const std::size_t kc : build_key_cols_) key_types_.push_back(build_types_[kc]);

  // Row layout: hash(8) | column values (packed) | null-flags (1B/col) | next(4).
  // Everything is accessed via memcpy, so no inter-field alignment is required.
  std::size_t off = 8;
  col_offsets_.reserve(build_types_.size());
  for (const TypeId t : build_types_) {
    col_offsets_.push_back(off);
    off += PhysicalSize(t);
  }
  null_offset_ = off;
  off += build_types_.size();
  next_offset_ = off;
  off += sizeof(std::uint32_t);
  row_width_ = Align8(off);

  slots_.assign(1024, kEmpty);
  mask_ = slots_.size() - 1;
}

std::uint32_t JoinHashTable::AppendRow(std::uint64_t hash, const DataChunk& chunk, std::size_t i) {
  const std::uint32_t r = num_rows_++;
  rows_.resize(rows_.size() + row_width_);  // zero-filled (null-flags default 0)
  std::byte* row = RowPtr(r);
  std::memcpy(row, &hash, sizeof(hash));
  for (std::size_t c = 0; c < build_types_.size(); ++c) {
    const Vector& col = chunk.column(c);
    std::byte* dst = row + col_offsets_[c];
    if (!col.validity().RowIsValid(i)) {
      row[null_offset_ + c] = std::byte{1};  // non-key build column may be NULL
      continue;
    }
    switch (build_types_[c]) {
      case TypeId::kInt32:
      case TypeId::kDate: {
        const std::int32_t v = col.Get<std::int32_t>(i);
        std::memcpy(dst, &v, sizeof(v));
        break;
      }
      case TypeId::kInt64: {
        const std::int64_t v = col.Get<std::int64_t>(i);
        std::memcpy(dst, &v, sizeof(v));
        break;
      }
      case TypeId::kDouble: {
        const double v = col.Get<double>(i);
        std::memcpy(dst, &v, sizeof(v));
        break;
      }
      case TypeId::kBool: {
        const std::uint8_t v = col.Get<std::uint8_t>(i);
        std::memcpy(dst, &v, sizeof(v));
        break;
      }
      case TypeId::kVarchar: {
        const StringRef sr = heap_.Add(col.Get<StringRef>(i).view());
        std::memcpy(dst, &sr, sizeof(sr));
        break;
      }
    }
  }
  const std::uint32_t empty = kEmpty;
  std::memcpy(row + next_offset_, &empty, sizeof(empty));  // next; set by the chain prepend
  return r;
}

void JoinHashTable::Grow() {
  slots_.assign(slots_.size() * 2, kEmpty);
  mask_ = slots_.size() - 1;
  for (std::uint32_t r = 0; r < num_rows_; ++r) {
    std::uint64_t h;
    std::memcpy(&h, RowPtr(r), sizeof(h));
    const std::size_t slot = static_cast<std::size_t>(h & mask_);
    std::memcpy(RowPtr(r) + next_offset_, &slots_[slot], sizeof(std::uint32_t));  // r.next = head
    slots_[slot] = r;
  }
}

void JoinHashTable::Insert(const DataChunk& chunk) {
  const std::size_t n = chunk.size();
  for (std::size_t i = 0; i < n; ++i) {
    if (AnyKeyNull(chunk, build_key_cols_, i)) continue;  // NULL build key never matches
    const std::uint64_t h = HashKeys(chunk, build_key_cols_, i);
    const std::uint32_t r = AppendRow(h, chunk, i);
    const std::size_t slot = static_cast<std::size_t>(h & mask_);
    std::memcpy(RowPtr(r) + next_offset_, &slots_[slot], sizeof(std::uint32_t));  // prepend
    slots_[slot] = r;
    if (num_rows_ > (slots_.size() * 7) / 10) Grow();
  }
}

bool JoinHashTable::AnyKeyNull(const DataChunk& chunk, const std::vector<std::size_t>& key_cols,
                               std::size_t i) const {
  for (const std::size_t kc : key_cols) {
    if (!chunk.column(kc).validity().RowIsValid(i)) return true;
  }
  return false;
}

std::uint64_t JoinHashTable::HashKeys(const DataChunk& chunk,
                                      const std::vector<std::size_t>& key_cols,
                                      std::size_t i) const {
  std::uint64_t h = hashing::kFnvSeed;
  for (std::size_t k = 0; k < key_cols.size(); ++k) {
    h = hashing::Combine(h, hashing::HashValue(key_types_[k], chunk.column(key_cols[k]), i));
  }
  return h;
}

std::uint32_t JoinHashTable::Next(std::uint32_t row) const {
  std::uint32_t n;
  std::memcpy(&n, RowPtr(row) + next_offset_, sizeof(n));
  return n;
}

std::uint64_t JoinHashTable::RowHash(std::uint32_t row) const {
  std::uint64_t h;
  std::memcpy(&h, RowPtr(row), sizeof(h));
  return h;
}

bool JoinHashTable::RowKeyEquals(std::uint32_t build_row, const DataChunk& probe,
                                 const std::vector<std::size_t>& probe_keys, std::size_t i) const {
  const std::byte* row = RowPtr(build_row);
  for (std::size_t k = 0; k < build_key_cols_.size(); ++k) {
    const std::byte* src = row + col_offsets_[build_key_cols_[k]];
    const Vector& pcol = probe.column(probe_keys[k]);
    switch (key_types_[k]) {
      case TypeId::kInt32:
      case TypeId::kDate: {
        std::int32_t v;
        std::memcpy(&v, src, sizeof(v));
        if (v != pcol.Get<std::int32_t>(i)) return false;
        break;
      }
      case TypeId::kInt64: {
        std::int64_t v;
        std::memcpy(&v, src, sizeof(v));
        if (v != pcol.Get<std::int64_t>(i)) return false;
        break;
      }
      case TypeId::kDouble: {
        double v;
        std::memcpy(&v, src, sizeof(v));
        if (v != pcol.Get<double>(i)) return false;
        break;
      }
      case TypeId::kBool: {
        std::uint8_t v;
        std::memcpy(&v, src, sizeof(v));
        if (v != pcol.Get<std::uint8_t>(i)) return false;
        break;
      }
      case TypeId::kVarchar: {
        StringRef sr;
        std::memcpy(&sr, src, sizeof(sr));
        if (sr.view() != pcol.Get<StringRef>(i).view()) return false;
        break;
      }
    }
  }
  return true;
}

void JoinHashTable::GatherBuild(const std::uint32_t* build_rows, std::size_t count, DataChunk& out,
                                std::size_t out_col_base) const {
  // Row-major: each matched row is read once, contiguously (the row-layout win).
  for (std::size_t j = 0; j < count; ++j) {
    const std::byte* row = RowPtr(build_rows[j]);
    for (std::size_t c = 0; c < build_types_.size(); ++c) {
      Vector& ocol = out.column(out_col_base + c);
      if (row[null_offset_ + c] != std::byte{0}) {
        ocol.SetNull(j);
        continue;
      }
      const std::byte* src = row + col_offsets_[c];
      switch (build_types_[c]) {
        case TypeId::kInt32:
        case TypeId::kDate: {
          std::int32_t v;
          std::memcpy(&v, src, sizeof(v));
          ocol.Set<std::int32_t>(j, v);
          break;
        }
        case TypeId::kInt64: {
          std::int64_t v;
          std::memcpy(&v, src, sizeof(v));
          ocol.Set<std::int64_t>(j, v);
          break;
        }
        case TypeId::kDouble: {
          double v;
          std::memcpy(&v, src, sizeof(v));
          ocol.Set<double>(j, v);
          break;
        }
        case TypeId::kBool: {
          std::uint8_t v;
          std::memcpy(&v, src, sizeof(v));
          ocol.Set<std::uint8_t>(j, v);
          break;
        }
        case TypeId::kVarchar: {
          StringRef sr;
          std::memcpy(&sr, src, sizeof(sr));
          ocol.Set<StringRef>(j, ocol.AddString(sr.view()));  // deep-copy into output heap
          break;
        }
      }
    }
  }
}

// ============================ HashJoinProbe ================================

HashJoinProbe::HashJoinProbe(const JoinHashTable& table, std::span<const TypeId> probe_types,
                             std::vector<std::size_t> probe_key_cols, Sink& output)
    : table_(&table),
      probe_key_cols_(std::move(probe_key_cols)),
      num_probe_cols_(probe_types.size()),
      output_(&output) {
  output_types_.reserve(probe_types.size() + table.build_column_count());
  for (const TypeId t : probe_types) output_types_.push_back(t);
  for (const TypeId t : table.build_types()) output_types_.push_back(t);
  match_probe_.InitOwned(kVectorSize);
  match_build_.resize(kVectorSize);
}

void HashJoinProbe::Flush(const DataChunk& probe_chunk, std::size_t count) {
  DataChunk out;
  out.Initialize(output_types_, kVectorSize);
  for (std::size_t c = 0; c < num_probe_cols_; ++c) {
    GatherColumn(probe_chunk.column(c), match_probe_, count, out.column(c));
  }
  table_->GatherBuild(match_build_.data(), count, out, num_probe_cols_);
  out.SetSize(count);
  output_->Consume(out);
}

void HashJoinProbe::Consume(const DataChunk& probe_chunk) {
  const std::size_t n = probe_chunk.size();
  std::size_t m = 0;
  for (std::size_t i = 0; i < n; ++i) {
    if (table_->AnyKeyNull(probe_chunk, probe_key_cols_, i)) continue;  // NULL probe key: no match
    const std::uint64_t h = table_->HashKeys(probe_chunk, probe_key_cols_, i);
    for (std::uint32_t r = table_->Head(h); r != JoinHashTable::kEmpty; r = table_->Next(r)) {
      if (table_->RowHash(r) != h) continue;  // quick reject on the full hash
      if (!table_->RowKeyEquals(r, probe_chunk, probe_key_cols_, i)) continue;
      match_probe_.Set(m, static_cast<sel_t>(i));
      match_build_[m] = r;
      ++m;
      if (m == kVectorSize) {
        Flush(probe_chunk, m);  // fan-out filled a batch; emit and continue
        m = 0;
      }
    }
  }
  if (m > 0) Flush(probe_chunk, m);
}

}  // namespace strata
