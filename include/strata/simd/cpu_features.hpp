#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace strata::simd {

// A snapshot of the SIMD capabilities Highway detected for THIS process on
// THIS CPU. We report it at startup (`strata --version`) so every benchmark
// number can be tied to the exact instruction set that produced it — a
// requirement for honest scalar-vs-SIMD and NEON-vs-AVX2 comparisons later.
struct SimdInfo {
  // The target Highway will actually dispatch to: the most capable instruction
  // set that is both compiled into the binary AND supported by the running CPU
  // (e.g. "NEON" on Apple Silicon, "AVX3"/"AVX2" on x86). "NONE" only if the
  // query somehow returned no targets, which should never happen.
  std::string dispatched_target;

  // All compiled-and-CPU-supported targets, in Highway's order of decreasing
  // preference (best first). Useful for showing the fallback ladder.
  std::vector<std::string> supported_targets;

  // Raw bitmask from hwy::SupportedTargets() — handy for logging/debugging.
  std::int64_t supported_target_bits = 0;
};

// Query Highway for the current process. Cheap; intended for startup/diagnostic
// use, not the hot path.
SimdInfo DetectSimd();

} // namespace strata::simd
