#include "strata/exec/filter.hpp"

#include <cstdint>

#include "strata/data/vector.hpp"

namespace strata {

std::size_t Filter::Select(const DataChunk& chunk, SelectionVector& out_sel) {
  const Vector result = executor_.Execute(*predicate_, chunk);  // BOOL value + validity
  const std::size_t n = chunk.size();
  out_sel.InitOwned(n);
  const std::uint8_t* vals = result.Data<std::uint8_t>();
  std::size_t count = 0;

  if (result.validity().AllValid()) {
    // No NULLs: a row passes iff its predicate value is TRUE.
    for (std::size_t i = 0; i < n; ++i) {
      if (vals[i] != 0) out_sel.Set(count++, static_cast<sel_t>(i));
    }
  } else {
    // 3VL: a NULL predicate (unknown) does NOT pass — only valid && TRUE does.
    for (std::size_t i = 0; i < n; ++i) {
      if (result.validity().RowIsValid(i) && vals[i] != 0) {
        out_sel.Set(count++, static_cast<sel_t>(i));
      }
    }
  }
  return count;
}

}  // namespace strata
