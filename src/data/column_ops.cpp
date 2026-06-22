#include "strata/data/column_ops.hpp"

#include <cstdint>

#include "strata/data/string_ref.hpp"
#include "strata/data/types.hpp"

namespace strata {

void CopyElement(const Vector& src, std::size_t si, Vector& dst, std::size_t di) {
  if (!src.validity().RowIsValid(si)) {
    dst.SetNull(di);
    return;
  }
  switch (src.type()) {
    case TypeId::kBool:
      dst.Set<std::uint8_t>(di, src.Get<std::uint8_t>(si));
      break;
    case TypeId::kInt32:
    case TypeId::kDate:
      dst.Set<std::int32_t>(di, src.Get<std::int32_t>(si));
      break;
    case TypeId::kInt64:
      dst.Set<std::int64_t>(di, src.Get<std::int64_t>(si));
      break;
    case TypeId::kDouble:
      dst.Set<double>(di, src.Get<double>(si));
      break;
    case TypeId::kVarchar:
      dst.Set<StringRef>(di, dst.AddString(src.Get<StringRef>(si).view()));
      break;
  }
}

void CopyColumn(const Vector& src, Vector& dst, std::size_t count) {
  for (std::size_t r = 0; r < count; ++r) CopyElement(src, r, dst, r);
}

void GatherColumn(const Vector& src, const SelectionVector& sel, std::size_t count,
                  Vector& dst) {
  for (std::size_t j = 0; j < count; ++j) CopyElement(src, sel.Get(j), dst, j);
}

}  // namespace strata
