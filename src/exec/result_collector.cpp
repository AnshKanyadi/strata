#include "strata/exec/result_collector.hpp"

#include <cstdint>
#include <format>
#include <span>
#include <string>

#include "strata/data/column_ops.hpp"
#include "strata/data/string_ref.hpp"
#include "strata/data/types.hpp"
#include "strata/data/vector.hpp"
#include "strata/util/date.hpp"

namespace strata {
namespace {

// Deep-copy a borrowed source chunk into freshly-owned storage. VARCHAR bytes
// are re-added into the destination's own StringHeap so the result is fully
// independent of the source (ADR 0006's borrow contract).
DataChunk CopyChunk(const DataChunk& src, std::span<const TypeId> types) {
  DataChunk dst;
  dst.Initialize(types, kVectorSize);
  const std::size_t n = src.size();
  for (std::size_t col = 0; col < src.ColumnCount(); ++col) {
    CopyColumn(src.column(col), dst.column(col), n);
  }
  dst.SetSize(n);
  return dst;
}

std::string FormatCell(const ResultCollector& rc, std::size_t row, std::size_t col) {
  if (rc.IsNull(row, col)) return "NULL";
  switch (rc.schema().column(col).type) {
    case TypeId::kBool:    return rc.Get<std::uint8_t>(row, col) != 0 ? "true" : "false";
    case TypeId::kInt32:   return std::format("{}", rc.Get<std::int32_t>(row, col));
    case TypeId::kInt64:   return std::format("{}", rc.Get<std::int64_t>(row, col));
    case TypeId::kDouble:  return std::format("{}", rc.Get<double>(row, col));
    case TypeId::kDate: {
      const Civil c = CivilFromDays(rc.Get<std::int32_t>(row, col));
      return std::format("{:04}-{:02}-{:02}", c.year, c.month, c.day);
    }
    case TypeId::kVarchar: return rc.GetString(row, col);
  }
  return "?";  // unreachable; satisfies -Wreturn-type
}

}  // namespace

void ResultCollector::Consume(const DataChunk& chunk) {
  if (chunk.size() == 0) return;
  chunks_.push_back(CopyChunk(chunk, schema_.types()));
  row_count_ += chunk.size();
}

std::pair<std::size_t, std::size_t> ResultCollector::Locate(std::size_t row) const {
  std::size_t remaining = row;
  for (std::size_t ci = 0; ci < chunks_.size(); ++ci) {
    const std::size_t sz = chunks_[ci].size();
    if (remaining < sz) return {ci, remaining};
    remaining -= sz;
  }
  return {0, 0};  // out of range (caller precondition: row < row_count())
}

bool ResultCollector::IsNull(std::size_t row, std::size_t col) const {
  const auto [ci, off] = Locate(row);
  return chunks_[ci].column(col).IsNull(off);
}

std::string ResultCollector::GetString(std::size_t row, std::size_t col) const {
  const auto [ci, off] = Locate(row);
  return std::string(chunks_[ci].column(col).Get<StringRef>(off).view());
}

void ResultCollector::Print(std::ostream& os) const {
  for (std::size_t c = 0; c < schema_.size(); ++c) {
    if (c != 0) os << " | ";
    os << schema_.column(c).name;
  }
  os << '\n';
  for (std::size_t r = 0; r < row_count_; ++r) {
    for (std::size_t c = 0; c < schema_.size(); ++c) {
      if (c != 0) os << " | ";
      os << FormatCell(*this, r, c);
    }
    os << '\n';
  }
}

}  // namespace strata
