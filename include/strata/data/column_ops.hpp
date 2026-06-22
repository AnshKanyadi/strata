#pragma once

#include <cstddef>

#include "strata/data/selection_vector.hpp"
#include "strata/data/vector.hpp"

namespace strata {

// Copy one value from src[si] to dst[di], honoring NULLs and re-adding VARCHAR
// bytes into dst's own StringHeap. src and dst must have the same TypeId.
void CopyElement(const Vector& src, std::size_t si, Vector& dst, std::size_t di);

// dst[0..count) = src[0..count). (Materializes a prefix of a column.)
void CopyColumn(const Vector& src, Vector& dst, std::size_t count);

// dst[j] = src[sel.Get(j)] for j in [0, count). The gather that compacts a
// selection into a dense output column (used by Project).
void GatherColumn(const Vector& src, const SelectionVector& sel, std::size_t count, Vector& dst);

}  // namespace strata
