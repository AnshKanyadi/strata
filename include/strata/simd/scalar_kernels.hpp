#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "strata/simd/kernels.hpp"  // ArithOp, CmpOp

// Hand-written, INDEPENDENT scalar reference for the SIMD kernels. Two jobs:
//   1. The differential-test oracle (ADR 0008): assert dispatched-SIMD output ==
//      scalar output on random inputs. Deliberately NOT Highway's scalar target,
//      so a Highway bug cannot be masked by testing Highway against itself.
//   2. The SIMD kernels' own scalar tail reuses these ops, so wrapping semantics
//      are identical between the vector body and the remainder.
namespace strata::simd::scalar {

// Integer +,-,* WRAP via unsigned (UB-free; matches Highway's two's-complement
// SIMD). Floating point is plain IEEE.
template <class T>
constexpr T AddOp(T a, T b) noexcept {
  if constexpr (std::is_integral_v<T>) {
    using U = std::make_unsigned_t<T>;
    return static_cast<T>(static_cast<U>(a) + static_cast<U>(b));
  } else {
    return a + b;
  }
}
template <class T>
constexpr T SubOp(T a, T b) noexcept {
  if constexpr (std::is_integral_v<T>) {
    using U = std::make_unsigned_t<T>;
    return static_cast<T>(static_cast<U>(a) - static_cast<U>(b));
  } else {
    return a - b;
  }
}
template <class T>
constexpr T MulOp(T a, T b) noexcept {
  if constexpr (std::is_integral_v<T>) {
    using U = std::make_unsigned_t<T>;
    return static_cast<T>(static_cast<U>(a) * static_cast<U>(b));
  } else {
    return a * b;
  }
}

template <class T>
constexpr T ApplyArith(ArithOp op, T a, T b) noexcept {
  switch (op) {
    case ArithOp::kAdd: return AddOp(a, b);
    case ArithOp::kSub: return SubOp(a, b);
    case ArithOp::kMul: return MulOp(a, b);
  }
  return T{};  // unreachable; satisfies -Wreturn-type
}

template <class T>
constexpr std::uint8_t ApplyCmp(CmpOp op, T a, T b) noexcept {
  bool r = false;
  switch (op) {
    case CmpOp::kEq: r = (a == b); break;
    case CmpOp::kNe: r = (a != b); break;
    case CmpOp::kLt: r = (a < b);  break;
    case CmpOp::kLe: r = (a <= b); break;
    case CmpOp::kGt: r = (a > b);  break;
    case CmpOp::kGe: r = (a >= b); break;
  }
  return r ? std::uint8_t{1} : std::uint8_t{0};
}

// Full-array scalar reference implementations.
template <class T>
void Arith(ArithOp op, const T* a, const T* b, T* out, std::size_t n) {
  for (std::size_t i = 0; i < n; ++i) out[i] = ApplyArith(op, a[i], b[i]);
}
template <class T>
void Compare(CmpOp op, const T* a, const T* b, std::uint8_t* out, std::size_t n) {
  for (std::size_t i = 0; i < n; ++i) out[i] = ApplyCmp(op, a[i], b[i]);
}

}  // namespace strata::simd::scalar
