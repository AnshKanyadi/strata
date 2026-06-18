#pragma once

#include <cstddef>

namespace strata {

// kVectorSize is THE central tuning constant of a vectorized engine: the number
// of values an operator processes per call (a "vector"/"chunk"/"batch"/morsel).
//
// We use 2048, following MonetDB/X100 and DuckDB. The reasoning is cache, not
// taste: per-call overhead (virtual dispatch, bounds setup, branch prediction
// warm-up) is amortized across the whole batch, so it must be large; but the
// operator's working set — a handful of these columns at a few bytes each —
// must stay resident in L1/L2 so the inner loops never stall on memory. At
// 2048 values an INT32 column is 8 KiB; a few such columns fit comfortably in a
// typical 128-256 KiB L2. Too small (e.g. 64) and dispatch overhead dominates;
// too large (e.g. 1<<20) and the working set spills out of cache, which is the
// exact pathology X100 was designed to avoid. Keep this the single knob.
inline constexpr std::size_t kVectorSize = 2048;

} // namespace strata
