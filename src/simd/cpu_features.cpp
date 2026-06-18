#include "strata/simd/cpu_features.hpp"

#include <hwy/targets.h>

namespace strata::simd {

SimdInfo DetectSimd() {
  SimdInfo info;
  info.supported_target_bits = hwy::SupportedTargets();

  // SupportedAndGeneratedTargets() returns the instruction sets that are both
  // (a) compiled into this binary and (b) supported by the running CPU, in
  // Highway's order of decreasing preference. Highway dispatches to the first.
  const std::vector<std::int64_t> targets = hwy::SupportedAndGeneratedTargets();
  info.supported_targets.reserve(targets.size());
  for (const std::int64_t target : targets) {
    info.supported_targets.emplace_back(hwy::TargetName(target));
  }

  info.dispatched_target =
      info.supported_targets.empty() ? "NONE" : info.supported_targets.front();
  return info;
}

} // namespace strata::simd
