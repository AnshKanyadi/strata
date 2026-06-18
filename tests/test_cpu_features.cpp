#include <gtest/gtest.h>

#include <algorithm>

#include "strata/config.hpp"
#include "strata/simd/cpu_features.hpp"

namespace strata {
namespace {

// These assertions are intentionally platform-agnostic: they must pass on both
// CI runners (NEON on macOS/arm64, AVX2/AVX3 on Linux/x86_64). We assert
// invariants, not a specific instruction set, to keep the test honest.

TEST(CpuFeatures, ReportsAtLeastOneTarget) {
  const simd::SimdInfo info = simd::DetectSimd();
  // Highway guarantees at least one supported target (scalar fallback exists).
  EXPECT_FALSE(info.supported_targets.empty());
  EXPECT_NE(info.supported_target_bits, 0);
}

TEST(CpuFeatures, DispatchedTargetIsTheFirstSupported) {
  const simd::SimdInfo info = simd::DetectSimd();
  ASSERT_FALSE(info.supported_targets.empty());
  EXPECT_EQ(info.dispatched_target, info.supported_targets.front());
  EXPECT_NE(info.dispatched_target, "NONE");
}

TEST(CpuFeatures, TargetNamesAreNonEmpty) {
  const simd::SimdInfo info = simd::DetectSimd();
  for (const auto& name : info.supported_targets) {
    EXPECT_FALSE(name.empty());
  }
}

TEST(Config, VectorSizeIsTheDuckDbSweetSpot) {
  // Documented invariant; if this ever changes it should be a deliberate,
  // reviewed decision (see config.hpp for the cache reasoning).
  static_assert(kVectorSize == 2048);
  EXPECT_EQ(kVectorSize, 2048u);
}

} // namespace
} // namespace strata
