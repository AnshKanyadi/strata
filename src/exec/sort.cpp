#include "strata/exec/sort.hpp"

#include <algorithm>
#include <cstdint>
#include <numeric>
#include <utility>

#include "strata/data/column_ops.hpp"
#include "strata/data/string_ref.hpp"
#include "strata/data/vector.hpp"

namespace strata {
namespace {

// Three-way value compare for two NON-NULL values of the given type.
int CompareValue(TypeId t, const Vector& a, std::size_t ra, const Vector& b, std::size_t rb) {
  switch (t) {
    case TypeId::kInt32:
    case TypeId::kDate: {
      const std::int32_t x = a.Get<std::int32_t>(ra), y = b.Get<std::int32_t>(rb);
      return (x < y) ? -1 : (x > y) ? 1 : 0;
    }
    case TypeId::kInt64: {
      const std::int64_t x = a.Get<std::int64_t>(ra), y = b.Get<std::int64_t>(rb);
      return (x < y) ? -1 : (x > y) ? 1 : 0;
    }
    case TypeId::kDouble: {
      const double x = a.Get<double>(ra), y = b.Get<double>(rb);
      return (x < y) ? -1 : (x > y) ? 1 : 0;
    }
    case TypeId::kBool: {
      const std::uint8_t x = a.Get<std::uint8_t>(ra), y = b.Get<std::uint8_t>(rb);
      return (x < y) ? -1 : (x > y) ? 1 : 0;
    }
    case TypeId::kVarchar: {
      // Bind by reference: Get<StringRef> returns a temporary, and for an inlined
      // (<=12 byte) string .view() would point into that temporary's bytes and
      // dangle once it is destroyed. Data<StringRef>()[r] refers to the stored ref.
      const StringRef& sa = a.Data<StringRef>()[ra];
      const StringRef& sb = b.Data<StringRef>()[rb];
      const std::string_view x = sa.view(), y = sb.view();
      return (x < y) ? -1 : (x > y) ? 1 : 0;
    }
  }
  return 0;
}

}  // namespace

int CompareRows(const DataChunk& a, std::size_t ra, const DataChunk& b, std::size_t rb,
                const std::vector<SortKey>& keys) {
  for (const SortKey& k : keys) {
    const Vector& ca = a.column(k.col);
    const Vector& cb = b.column(k.col);
    const bool an = !ca.validity().RowIsValid(ra);
    const bool bn = !cb.validity().RowIsValid(rb);
    if (an && bn) continue;
    if (an != bn) {
      // NULL placement is absolute (not flipped by ASC/DESC): the null operand
      // sorts first iff nulls_first.
      if (an) return k.nulls_first ? -1 : 1;
      return k.nulls_first ? 1 : -1;
    }
    const int c = CompareValue(k.type, ca, ra, cb, rb);
    if (c != 0) return k.ascending ? c : -c;  // ASC/DESC flips only the value compare
  }
  return 0;
}

Sort::Sort(std::vector<SortKey> keys, Sink& output) : keys_(std::move(keys)), output_(&output) {}

void Sort::Append(const DataChunk& src, std::size_t i) {
  if (types_.empty()) {
    for (std::size_t c = 0; c < src.ColumnCount(); ++c) types_.push_back(src.column(c).type());
  }
  const std::size_t slot = total_ % kVectorSize;
  if (slot == 0) {
    DataChunk c;
    c.Initialize(types_, kVectorSize);
    chunks_.push_back(std::move(c));
  }
  DataChunk& dst = chunks_.back();
  for (std::size_t c = 0; c < types_.size(); ++c) {
    CopyElement(src.column(c), i, dst.column(c), slot);
  }
  dst.SetSize(slot + 1);
  ++total_;
}

void Sort::Consume(const DataChunk& chunk) {
  const std::size_t n = chunk.size();
  for (std::size_t i = 0; i < n; ++i) Append(chunk, i);
}

void Sort::Finalize() {
  std::vector<std::uint32_t> idx(total_);
  std::iota(idx.begin(), idx.end(), std::uint32_t{0});
  std::stable_sort(idx.begin(), idx.end(), [&](std::uint32_t a, std::uint32_t b) {
    const std::size_t ca = a / kVectorSize, oa = a % kVectorSize;
    const std::size_t cb = b / kVectorSize, ob = b % kVectorSize;
    return CompareRows(chunks_[ca], oa, chunks_[cb], ob, keys_) < 0;
  });

  DataChunk out;
  std::size_t produced = 0;
  while (produced < total_) {
    const std::size_t batch = std::min(kVectorSize, total_ - produced);
    out.Initialize(types_, kVectorSize);
    for (std::size_t j = 0; j < batch; ++j) {
      const std::uint32_t g = idx[produced + j];
      const DataChunk& srcc = chunks_[g / kVectorSize];
      const std::size_t off = g % kVectorSize;
      for (std::size_t c = 0; c < types_.size(); ++c) {
        CopyElement(srcc.column(c), off, out.column(c), j);
      }
    }
    out.SetSize(batch);
    output_->Consume(out);
    produced += batch;
  }
  output_->Finalize();
}

}  // namespace strata
