// SIMD kernels via Google Highway with runtime dispatch (ADR 0008). This source
// is compiled once per SIMD target by foreach_target.h; HWY_EXPORT registers the
// per-target functions and HWY_DYNAMIC_DISPATCH selects the CPU-detected one at
// runtime, so the same source emits NEON on arm64 and AVX2 on x86.

#include "strata/simd/kernels.hpp"

#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "strata/simd/scalar_kernels.hpp"  // scalar::ApplyArith/ApplyCmp for the tail

// The path foreach_target.h re-includes; resolved via the `src` include dir.
#undef HWY_TARGET_INCLUDE
#define HWY_TARGET_INCLUDE "simd/kernels.cc"
#include "hwy/foreach_target.h"  // must precede highway.h
#include "hwy/highway.h"

HWY_BEFORE_NAMESPACE();
namespace strata::simd::HWY_NAMESPACE {
namespace hn = hwy::HWY_NAMESPACE;

// One arithmetic op over a flat array: SIMD body + scalar (wrapping) tail. `vop`
// is hoisted out of the loop, so the inner loop is branch-free.
template <class T, class VecOp>
void ArithLoop(const T* a, const T* b, T* out, std::size_t n, ArithOp op, VecOp vop) {
  const hn::ScalableTag<T> d;
  const std::size_t N = hn::Lanes(d);
  std::size_t i = 0;
  for (; i + N <= n; i += N) {
    hn::Store(vop(hn::Load(d, a + i), hn::Load(d, b + i)), d, out + i);
  }
  for (; i < n; ++i) out[i] = scalar::ApplyArith(op, a[i], b[i]);  // remainder
}

template <class T>
void ArithImpl(ArithOp op, const T* a, const T* b, T* out, std::size_t n) {
  switch (op) {  // op switch hoisted above the hot loop (once per batch)
    case ArithOp::kAdd:
      ArithLoop<T>(a, b, out, n, op, [](auto x, auto y) { return hn::Add(x, y); });
      break;
    case ArithOp::kSub:
      ArithLoop<T>(a, b, out, n, op, [](auto x, auto y) { return hn::Sub(x, y); });
      break;
    case ArithOp::kMul:
      ArithLoop<T>(a, b, out, n, op, [](auto x, auto y) { return hn::Mul(x, y); });
      break;
  }
}

// One comparison op over a flat array, emitting uint8 0/1. The wide lane mask is
// narrowed to bytes via DemoteTo (ADR 0008): integers go straight through;
// double routes its mask through the same-lane-count signed-int domain first.
template <class T, class CmpFn>
void CmpLoop(const T* a, const T* b, std::uint8_t* out, std::size_t n, CmpOp op, CmpFn cmp) {
  const hn::ScalableTag<T> d;
  const std::size_t N = hn::Lanes(d);
  std::size_t i = 0;
  for (; i + N <= n; i += N) {
    const auto m = cmp(hn::Load(d, a + i), hn::Load(d, b + i));
    if constexpr (std::is_integral_v<T>) {
      const hn::Rebind<std::uint8_t, hn::ScalableTag<T>> du8;
      hn::StoreU(hn::DemoteTo(du8, hn::IfThenElseZero(m, hn::Set(d, T{1}))), du8, out + i);
    } else {
      const hn::RebindToSigned<hn::ScalableTag<T>> di;  // i64 lanes, same count
      const hn::Rebind<std::uint8_t, decltype(di)> du8;
      const auto ones = hn::IfThenElseZero(hn::RebindMask(di, m), hn::Set(di, std::int64_t{1}));
      hn::StoreU(hn::DemoteTo(du8, ones), du8, out + i);
    }
  }
  for (; i < n; ++i) out[i] = scalar::ApplyCmp(op, a[i], b[i]);  // remainder
}

template <class T>
void CmpImpl(CmpOp op, const T* a, const T* b, std::uint8_t* out, std::size_t n) {
  switch (op) {
    case CmpOp::kEq: CmpLoop<T>(a, b, out, n, op, [](auto x, auto y) { return hn::Eq(x, y); }); break;
    case CmpOp::kNe: CmpLoop<T>(a, b, out, n, op, [](auto x, auto y) { return hn::Ne(x, y); }); break;
    case CmpOp::kLt: CmpLoop<T>(a, b, out, n, op, [](auto x, auto y) { return hn::Lt(x, y); }); break;
    case CmpOp::kLe: CmpLoop<T>(a, b, out, n, op, [](auto x, auto y) { return hn::Le(x, y); }); break;
    case CmpOp::kGt: CmpLoop<T>(a, b, out, n, op, [](auto x, auto y) { return hn::Gt(x, y); }); break;
    case CmpOp::kGe: CmpLoop<T>(a, b, out, n, op, [](auto x, auto y) { return hn::Ge(x, y); }); break;
  }
}

// Concrete, non-template entry points so HWY_EXPORT can register them.
void ArithI32(ArithOp op, const std::int32_t* a, const std::int32_t* b, std::int32_t* o, std::size_t n) { ArithImpl(op, a, b, o, n); }
void ArithI64(ArithOp op, const std::int64_t* a, const std::int64_t* b, std::int64_t* o, std::size_t n) { ArithImpl(op, a, b, o, n); }
void ArithF64(ArithOp op, const double* a, const double* b, double* o, std::size_t n) { ArithImpl(op, a, b, o, n); }
void CmpI32(CmpOp op, const std::int32_t* a, const std::int32_t* b, std::uint8_t* o, std::size_t n) { CmpImpl(op, a, b, o, n); }
void CmpI64(CmpOp op, const std::int64_t* a, const std::int64_t* b, std::uint8_t* o, std::size_t n) { CmpImpl(op, a, b, o, n); }
void CmpF64(CmpOp op, const double* a, const double* b, std::uint8_t* o, std::size_t n) { CmpImpl(op, a, b, o, n); }

}  // namespace strata::simd::HWY_NAMESPACE
HWY_AFTER_NAMESPACE();

#if HWY_ONCE
namespace strata::simd {

HWY_EXPORT(ArithI32);
HWY_EXPORT(ArithI64);
HWY_EXPORT(ArithF64);
HWY_EXPORT(CmpI32);
HWY_EXPORT(CmpI64);
HWY_EXPORT(CmpF64);

void Arith(ArithOp op, const std::int32_t* a, const std::int32_t* b, std::int32_t* out, std::size_t n) {
  HWY_DYNAMIC_DISPATCH(ArithI32)(op, a, b, out, n);
}
void Arith(ArithOp op, const std::int64_t* a, const std::int64_t* b, std::int64_t* out, std::size_t n) {
  HWY_DYNAMIC_DISPATCH(ArithI64)(op, a, b, out, n);
}
void Arith(ArithOp op, const double* a, const double* b, double* out, std::size_t n) {
  HWY_DYNAMIC_DISPATCH(ArithF64)(op, a, b, out, n);
}
void Compare(CmpOp op, const std::int32_t* a, const std::int32_t* b, std::uint8_t* out, std::size_t n) {
  HWY_DYNAMIC_DISPATCH(CmpI32)(op, a, b, out, n);
}
void Compare(CmpOp op, const std::int64_t* a, const std::int64_t* b, std::uint8_t* out, std::size_t n) {
  HWY_DYNAMIC_DISPATCH(CmpI64)(op, a, b, out, n);
}
void Compare(CmpOp op, const double* a, const double* b, std::uint8_t* out, std::size_t n) {
  HWY_DYNAMIC_DISPATCH(CmpF64)(op, a, b, out, n);
}

}  // namespace strata::simd
#endif  // HWY_ONCE
