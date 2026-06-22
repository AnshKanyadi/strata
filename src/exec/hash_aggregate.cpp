#include "strata/exec/hash_aggregate.hpp"

#include <algorithm>
#include <bit>
#include <cassert>
#include <cstring>
#include <string_view>
#include <utility>

#include "strata/data/string_ref.hpp"
#include "strata/data/vector.hpp"
#include "strata/exec/hash_util.hpp"  // hashing::Mix/Combine/HashValue

namespace strata {
namespace {

constexpr std::size_t Align8(std::size_t x) { return (x + 7) & ~std::size_t{7}; }
constexpr std::uint64_t kNullKeyHash = 0x9ddfea08eb382d69ULL;  // fixed value for NULL group keys

}  // namespace

HashAggregate::HashAggregate(std::vector<GroupKey> keys, std::vector<AggregateSpec> aggs,
                             Sink& output)
    : keys_(std::move(keys)), agg_specs_(std::move(aggs)), output_(&output) {
  aggs_.reserve(agg_specs_.size());
  for (const AggregateSpec& s : agg_specs_) {
    const ResolvedAggregate r = ResolveAggregate(s.func, s.input_type);
    assert(r.update != nullptr && r.finalize != nullptr && "unsupported aggregate/type");
    aggs_.push_back(r);
  }

  // Row layout: hash(8) | keys | null-flags | (8-aligned) states.
  std::size_t off = 8;
  key_offsets_.reserve(keys_.size());
  for (const GroupKey& k : keys_) {
    key_offsets_.push_back(off);
    off += PhysicalSize(k.type);
  }
  null_offset_ = off;
  off += keys_.size();  // one null-flag byte per key
  state_offsets_.reserve(aggs_.size());
  for (const ResolvedAggregate& a : aggs_) {
    off = Align8(off);
    state_offsets_.push_back(off);
    off += a.state_size;
  }
  row_width_ = Align8(off);

  output_types_.reserve(keys_.size() + aggs_.size());
  for (const GroupKey& k : keys_) output_types_.push_back(k.type);
  for (const ResolvedAggregate& a : aggs_) output_types_.push_back(a.output_type);

  slots_.assign(1024, Slot{kEmpty, 0});
  mask_ = slots_.size() - 1;

  // Global (no-key) aggregation: a single group exists from the start, so an
  // empty input still yields one result row (COUNT=0, SUM=NULL, ...).
  if (keys_.empty()) {
    AppendGroup(0);
  }
}

std::uint32_t HashAggregate::AppendGroup(std::uint64_t hash) {
  const std::uint32_t gi = num_groups_++;
  rows_.resize(rows_.size() + row_width_, std::byte{0});  // zero-init => states initialized
  std::memcpy(RowPtr(gi), &hash, sizeof(hash));
  return gi;
}

void HashAggregate::Grow() {
  slots_.assign(slots_.size() * 2, Slot{kEmpty, 0});
  mask_ = slots_.size() - 1;
  for (std::uint32_t gi = 0; gi < num_groups_; ++gi) {
    std::uint64_t h;
    std::memcpy(&h, RowPtr(gi), sizeof(h));
    std::size_t slot = static_cast<std::size_t>(h) & mask_;
    while (slots_[slot].index != kEmpty) slot = (slot + 1) & mask_;
    slots_[slot] = Slot{gi, static_cast<std::uint16_t>(h >> 48)};
  }
}

std::uint64_t HashAggregate::HashRow(const DataChunk& chunk, std::size_t row) const {
  std::uint64_t h = hashing::kFnvSeed;
  for (const GroupKey& k : keys_) {
    const Vector& col = chunk.column(k.col);
    const std::uint64_t kh =
        col.validity().RowIsValid(row) ? hashing::HashValue(k.type, col, row) : kNullKeyHash;
    h = hashing::Combine(h, kh);
  }
  return h;
}

void HashAggregate::WriteKeys(std::byte* row, const DataChunk& chunk, std::size_t i) {
  for (std::size_t k = 0; k < keys_.size(); ++k) {
    const Vector& col = chunk.column(keys_[k].col);
    std::byte* dst = row + key_offsets_[k];
    if (!col.validity().RowIsValid(i)) {
      row[null_offset_ + k] = std::byte{1};  // NULL key; value bytes stay zero
      continue;
    }
    row[null_offset_ + k] = std::byte{0};
    switch (keys_[k].type) {
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
        const StringRef sr = key_heap_.Add(col.Get<StringRef>(i).view());  // own the bytes
        std::memcpy(dst, &sr, sizeof(sr));
        break;
      }
    }
  }
}

bool HashAggregate::KeysEqual(const std::byte* row, const DataChunk& chunk, std::size_t i) const {
  for (std::size_t k = 0; k < keys_.size(); ++k) {
    const Vector& col = chunk.column(keys_[k].col);
    const bool incoming_null = !col.validity().RowIsValid(i);
    const bool stored_null = row[null_offset_ + k] != std::byte{0};
    if (incoming_null != stored_null) return false;
    if (incoming_null) continue;  // NULL == NULL for grouping
    const std::byte* src = row + key_offsets_[k];
    switch (keys_[k].type) {
      case TypeId::kInt32:
      case TypeId::kDate: {
        std::int32_t v;
        std::memcpy(&v, src, sizeof(v));
        if (v != col.Get<std::int32_t>(i)) return false;
        break;
      }
      case TypeId::kInt64: {
        std::int64_t v;
        std::memcpy(&v, src, sizeof(v));
        if (v != col.Get<std::int64_t>(i)) return false;
        break;
      }
      case TypeId::kDouble: {
        double v;
        std::memcpy(&v, src, sizeof(v));
        if (v != col.Get<double>(i)) return false;
        break;
      }
      case TypeId::kBool: {
        std::uint8_t v;
        std::memcpy(&v, src, sizeof(v));
        if (v != col.Get<std::uint8_t>(i)) return false;
        break;
      }
      case TypeId::kVarchar: {
        StringRef sr;
        std::memcpy(&sr, src, sizeof(sr));
        if (sr.view() != col.Get<StringRef>(i).view()) return false;
        break;
      }
    }
  }
  return true;
}

std::uint32_t HashAggregate::FindOrCreateGroup(const DataChunk& chunk, std::size_t row,
                                               std::uint64_t hash) {
  const std::uint16_t salt = static_cast<std::uint16_t>(hash >> 48);
  std::size_t slot = static_cast<std::size_t>(hash) & mask_;
  for (;;) {
    const Slot s = slots_[slot];
    if (s.index == kEmpty) {
      const std::uint32_t gi = AppendGroup(hash);
      WriteKeys(RowPtr(gi), chunk, row);
      slots_[slot] = Slot{gi, salt};
      if (num_groups_ > (slots_.size() * 7) / 10) Grow();  // keep load factor < 0.7
      return gi;
    }
    if (s.salt == salt && KeysEqual(RowPtr(s.index), chunk, row)) return s.index;
    slot = (slot + 1) & mask_;
  }
}

void HashAggregate::Consume(const DataChunk& chunk) {
  const std::size_t n = chunk.size();
  if (n == 0) return;

  // 1. Resolve a group index for every row (this is the only phase that inserts,
  //    so the rows buffer is stable during the update phase below).
  group_idx_.resize(n);
  if (keys_.empty()) {
    for (std::size_t i = 0; i < n; ++i) group_idx_[i] = 0;  // the single global group
  } else {
    for (std::size_t i = 0; i < n; ++i) {
      group_idx_[i] = FindOrCreateGroup(chunk, i, HashRow(chunk, i));
    }
  }

  // 2. Scatter-update each aggregate's state once per batch (loop is internal).
  std::byte* rows = rows_.data();
  for (std::size_t a = 0; a < aggs_.size(); ++a) {
    const Vector* col = aggs_[a].reads_input ? &chunk.column(agg_specs_[a].input_col) : nullptr;
    aggs_[a].update(rows, row_width_, state_offsets_[a], group_idx_.data(), col, n);
  }
}

void HashAggregate::WriteKeyToOutput(Vector& out, std::size_t out_row, const std::byte* row,
                                     std::size_t k) const {
  if (row[null_offset_ + k] != std::byte{0}) {
    out.SetNull(out_row);
    return;
  }
  const std::byte* src = row + key_offsets_[k];
  switch (keys_[k].type) {
    case TypeId::kInt32:
    case TypeId::kDate: {
      std::int32_t v;
      std::memcpy(&v, src, sizeof(v));
      out.Set<std::int32_t>(out_row, v);
      break;
    }
    case TypeId::kInt64: {
      std::int64_t v;
      std::memcpy(&v, src, sizeof(v));
      out.Set<std::int64_t>(out_row, v);
      break;
    }
    case TypeId::kDouble: {
      double v;
      std::memcpy(&v, src, sizeof(v));
      out.Set<double>(out_row, v);
      break;
    }
    case TypeId::kBool: {
      std::uint8_t v;
      std::memcpy(&v, src, sizeof(v));
      out.Set<std::uint8_t>(out_row, v);
      break;
    }
    case TypeId::kVarchar: {
      StringRef sr;
      std::memcpy(&sr, src, sizeof(sr));
      out.Set<StringRef>(out_row, out.AddString(sr.view()));  // deep-copy into output heap
      break;
    }
  }
}

void HashAggregate::Finalize() {
  DataChunk out;
  std::uint32_t produced = 0;
  while (produced < num_groups_) {
    const std::uint32_t batch =
        static_cast<std::uint32_t>(std::min<std::size_t>(kVectorSize, num_groups_ - produced));
    out.Initialize(output_types_, kVectorSize);
    for (std::uint32_t j = 0; j < batch; ++j) {
      const std::byte* row = RowPtr(produced + j);
      for (std::size_t k = 0; k < keys_.size(); ++k) {
        WriteKeyToOutput(out.column(k), j, row, k);
      }
      for (std::size_t a = 0; a < aggs_.size(); ++a) {
        aggs_[a].finalize(row + state_offsets_[a], out.column(keys_.size() + a), j);
      }
    }
    out.SetSize(batch);
    output_->Consume(out);
    produced += batch;
  }
  output_->Finalize();
}

}  // namespace strata
