#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <random>
#include <vector>

#include "strata/simd/kernels.hpp"
#include "strata/simd/scalar_kernels.hpp"

// Differential test (ADR 0008): the dispatched SIMD kernel must produce exactly
// the same output as the independent hand-written scalar reference, for every
// op and type, across many sizes — crucially including sizes that are NOT a
// multiple of the SIMD lane width, so the scalar tail is exercised. Inputs span
// the full value range, so integer wrapping (which both paths share) is tested.
namespace strata::simd {
namespace {

// Sizes chosen to straddle NEON (4xi32 / 2xi64,f64) and AVX2 (8xi32 / 4xi64,f64)
// lane boundaries, plus full-batch and multi-batch sizes.
const std::vector<std::size_t> kSizes = {0,  1,  2,   3,    4,    5,    7,   8,
                                         9,  15, 16,  17,   31,   33,   63,  64,
                                         65, 127, 256, 1000, 2048, 2049, 4097};

template <class T>
T RandVal(std::mt19937_64& rng) {
  if constexpr (std::is_integral_v<T>) {
    return std::uniform_int_distribution<T>(std::numeric_limits<T>::min(),
                                            std::numeric_limits<T>::max())(rng);
  } else {
    return std::uniform_real_distribution<double>(-1.0e6, 1.0e6)(rng);
  }
}

template <class T>
void CheckArith(ArithOp op) {
  std::mt19937_64 rng(0x5314A2u);
  for (const std::size_t n : kSizes) {
    std::vector<T> a(n), b(n), expected(n), got(n);
    for (std::size_t i = 0; i < n; ++i) {
      a[i] = RandVal<T>(rng);
      b[i] = RandVal<T>(rng);
    }
    scalar::Arith(op, a.data(), b.data(), expected.data(), n);
    Arith(op, a.data(), b.data(), got.data(), n);
    for (std::size_t i = 0; i < n; ++i) {
      ASSERT_EQ(expected[i], got[i]) << "n=" << n << " i=" << i;
    }
  }
}

template <class T>
void CheckCompare(CmpOp op) {
  std::mt19937_64 rng(0x9E3779B9u);
  for (const std::size_t n : kSizes) {
    std::vector<T> a(n), b(n);
    std::vector<std::uint8_t> expected(n), got(n);
    for (std::size_t i = 0; i < n; ++i) {
      a[i] = RandVal<T>(rng);
      b[i] = (i % 4 == 0) ? a[i] : RandVal<T>(rng);  // seed some equal pairs
    }
    scalar::Compare(op, a.data(), b.data(), expected.data(), n);
    Compare(op, a.data(), b.data(), got.data(), n);
    for (std::size_t i = 0; i < n; ++i) {
      ASSERT_EQ(int{expected[i]}, int{got[i]}) << "n=" << n << " i=" << i;
    }
  }
}

TEST(Kernels, ArithInt32) {
  CheckArith<std::int32_t>(ArithOp::kAdd);
  CheckArith<std::int32_t>(ArithOp::kSub);
  CheckArith<std::int32_t>(ArithOp::kMul);
}
TEST(Kernels, ArithInt64) {
  CheckArith<std::int64_t>(ArithOp::kAdd);
  CheckArith<std::int64_t>(ArithOp::kSub);
  CheckArith<std::int64_t>(ArithOp::kMul);
}
TEST(Kernels, ArithDouble) {
  CheckArith<double>(ArithOp::kAdd);
  CheckArith<double>(ArithOp::kSub);
  CheckArith<double>(ArithOp::kMul);
}

TEST(Kernels, CompareInt32) {
  for (const CmpOp op : {CmpOp::kEq, CmpOp::kNe, CmpOp::kLt, CmpOp::kLe, CmpOp::kGt, CmpOp::kGe})
    CheckCompare<std::int32_t>(op);
}
TEST(Kernels, CompareInt64) {
  for (const CmpOp op : {CmpOp::kEq, CmpOp::kNe, CmpOp::kLt, CmpOp::kLe, CmpOp::kGt, CmpOp::kGe})
    CheckCompare<std::int64_t>(op);
}
TEST(Kernels, CompareDouble) {
  for (const CmpOp op : {CmpOp::kEq, CmpOp::kNe, CmpOp::kLt, CmpOp::kLe, CmpOp::kGt, CmpOp::kGe})
    CheckCompare<double>(op);
}

TEST(Kernels, KnownValues) {
  const std::int32_t a[5] = {1, 5, 3, 8, 2};
  const std::int32_t b[5] = {4, 4, 4, 4, 4};
  std::int32_t sum[5];
  std::uint8_t gt[5];
  Arith(ArithOp::kAdd, a, b, sum, 5);
  Compare(CmpOp::kGt, a, b, gt, 5);
  EXPECT_EQ(sum[3], 12);
  EXPECT_EQ(int{gt[0]}, 0);  // 1 > 4
  EXPECT_EQ(int{gt[1]}, 1);  // 5 > 4
  EXPECT_EQ(int{gt[3]}, 1);  // 8 > 4
}

}  // namespace
}  // namespace strata::simd
