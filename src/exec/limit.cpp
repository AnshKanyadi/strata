#include "strata/exec/limit.hpp"

#include <algorithm>
#include <cstddef>
#include <vector>

#include "strata/data/column_ops.hpp"
#include "strata/data/types.hpp"
#include "strata/data/vector.hpp"

namespace strata {

void Limit::Consume(const DataChunk& src) {
  if (emitted_ >= n_) return;
  const std::size_t take = std::min(n_ - emitted_, src.size());
  if (take == 0) return;

  std::vector<TypeId> types;
  types.reserve(src.ColumnCount());
  for (std::size_t c = 0; c < src.ColumnCount(); ++c) types.push_back(src.column(c).type());

  DataChunk out;
  out.Initialize(types, kVectorSize);
  for (std::size_t c = 0; c < types.size(); ++c) {
    CopyColumn(src.column(c), out.column(c), take);  // forward the first `take` rows
  }
  out.SetSize(take);
  output_->Consume(out);
  emitted_ += take;
}

}  // namespace strata
