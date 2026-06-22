#include "strata/exec/aggregate.hpp"

#include <cstddef>
#include <cstdint>

#include "strata/data/types.hpp"
#include "strata/data/vector.hpp"
#include "strata/simd/scalar_kernels.hpp"  // scalar::AddOp (wrapping integer add)

namespace strata {
namespace {

// State structs stored inline in the group row. Zero bytes == initial state.
struct CountSt {
  std::int64_t count;
};
template <class A>
struct SumSt {
  A sum;
  bool has;
};
template <class T>
struct MinMaxSt {
  T value;
  bool has;
};
template <class A>
struct AvgSt {
  A sum;
  std::int64_t count;
};

template <class T>
std::byte* StateAt(std::byte* rows, std::size_t rw, std::size_t off, std::uint32_t gi) {
  return rows + static_cast<std::size_t>(gi) * rw + off;
}

// --- update kernels (per batch; loop internal, no per-row dispatch) ---

void UpdCountStar(std::byte* rows, std::size_t rw, std::size_t off, const std::uint32_t* g,
                  const Vector* /*col*/, std::size_t n) {
  for (std::size_t i = 0; i < n; ++i) {
    reinterpret_cast<CountSt*>(StateAt<CountSt>(rows, rw, off, g[i]))->count += 1;
  }
}
void UpdCount(std::byte* rows, std::size_t rw, std::size_t off, const std::uint32_t* g,
              const Vector* col, std::size_t n) {
  for (std::size_t i = 0; i < n; ++i) {
    if (!col->validity().RowIsValid(i)) continue;  // COUNT(col): non-NULL only
    reinterpret_cast<CountSt*>(StateAt<CountSt>(rows, rw, off, g[i]))->count += 1;
  }
}
template <class A, class T>
void UpdSum(std::byte* rows, std::size_t rw, std::size_t off, const std::uint32_t* g,
            const Vector* col, std::size_t n) {
  for (std::size_t i = 0; i < n; ++i) {
    if (!col->validity().RowIsValid(i)) continue;
    auto* st = reinterpret_cast<SumSt<A>*>(StateAt<SumSt<A>>(rows, rw, off, g[i]));
    st->sum = simd::scalar::AddOp<A>(st->sum, static_cast<A>(col->Get<T>(i)));  // wraps if integral
    st->has = true;
  }
}
template <class T, bool IsMax>
void UpdMinMax(std::byte* rows, std::size_t rw, std::size_t off, const std::uint32_t* g,
               const Vector* col, std::size_t n) {
  for (std::size_t i = 0; i < n; ++i) {
    if (!col->validity().RowIsValid(i)) continue;
    auto* st = reinterpret_cast<MinMaxSt<T>*>(StateAt<MinMaxSt<T>>(rows, rw, off, g[i]));
    const T v = col->Get<T>(i);
    if (!st->has || (IsMax ? (v > st->value) : (v < st->value))) st->value = v;
    st->has = true;
  }
}
template <class A, class T>
void UpdAvg(std::byte* rows, std::size_t rw, std::size_t off, const std::uint32_t* g,
            const Vector* col, std::size_t n) {
  for (std::size_t i = 0; i < n; ++i) {
    if (!col->validity().RowIsValid(i)) continue;
    auto* st = reinterpret_cast<AvgSt<A>*>(StateAt<AvgSt<A>>(rows, rw, off, g[i]));
    st->sum = simd::scalar::AddOp<A>(st->sum, static_cast<A>(col->Get<T>(i)));
    st->count += 1;
  }
}

// --- finalize ---

void FinCount(const std::byte* s, Vector& out, std::size_t r) {
  out.Set<std::int64_t>(r, reinterpret_cast<const CountSt*>(s)->count);
}
template <class A, class OutT>
void FinSum(const std::byte* s, Vector& out, std::size_t r) {
  const auto* st = reinterpret_cast<const SumSt<A>*>(s);
  if (!st->has) out.SetNull(r);
  else out.Set<OutT>(r, static_cast<OutT>(st->sum));
}
template <class T>
void FinMinMax(const std::byte* s, Vector& out, std::size_t r) {
  const auto* st = reinterpret_cast<const MinMaxSt<T>*>(s);
  if (!st->has) out.SetNull(r);
  else out.Set<T>(r, st->value);
}
template <class A>
void FinAvg(const std::byte* s, Vector& out, std::size_t r) {
  const auto* st = reinterpret_cast<const AvgSt<A>*>(s);
  if (st->count == 0) out.SetNull(r);
  else out.Set<double>(r, static_cast<double>(st->sum) / static_cast<double>(st->count));
}

}  // namespace

ResolvedAggregate ResolveAggregate(AggFunc func, TypeId t) {
  using R = ResolvedAggregate;
  switch (func) {
    case AggFunc::kCountStar:
      return R{sizeof(CountSt), TypeId::kInt64, false, &UpdCountStar, &FinCount};
    case AggFunc::kCount:
      return R{sizeof(CountSt), TypeId::kInt64, true, &UpdCount, &FinCount};
    case AggFunc::kSum:
      switch (t) {
        case TypeId::kInt32:
          return R{sizeof(SumSt<std::int64_t>), TypeId::kInt64, true, &UpdSum<std::int64_t, std::int32_t>, &FinSum<std::int64_t, std::int64_t>};
        case TypeId::kInt64:
          return R{sizeof(SumSt<std::int64_t>), TypeId::kInt64, true, &UpdSum<std::int64_t, std::int64_t>, &FinSum<std::int64_t, std::int64_t>};
        case TypeId::kDouble:
          return R{sizeof(SumSt<double>), TypeId::kDouble, true, &UpdSum<double, double>, &FinSum<double, double>};
        default:
          return R{};  // unsupported -> null fns (asserted at setup)
      }
    case AggFunc::kMin:
      switch (t) {
        case TypeId::kInt32:
        case TypeId::kDate:
          return R{sizeof(MinMaxSt<std::int32_t>), t, true, &UpdMinMax<std::int32_t, false>, &FinMinMax<std::int32_t>};
        case TypeId::kInt64:
          return R{sizeof(MinMaxSt<std::int64_t>), t, true, &UpdMinMax<std::int64_t, false>, &FinMinMax<std::int64_t>};
        case TypeId::kDouble:
          return R{sizeof(MinMaxSt<double>), t, true, &UpdMinMax<double, false>, &FinMinMax<double>};
        default:
          return R{};
      }
    case AggFunc::kMax:
      switch (t) {
        case TypeId::kInt32:
        case TypeId::kDate:
          return R{sizeof(MinMaxSt<std::int32_t>), t, true, &UpdMinMax<std::int32_t, true>, &FinMinMax<std::int32_t>};
        case TypeId::kInt64:
          return R{sizeof(MinMaxSt<std::int64_t>), t, true, &UpdMinMax<std::int64_t, true>, &FinMinMax<std::int64_t>};
        case TypeId::kDouble:
          return R{sizeof(MinMaxSt<double>), t, true, &UpdMinMax<double, true>, &FinMinMax<double>};
        default:
          return R{};
      }
    case AggFunc::kAvg:
      switch (t) {
        case TypeId::kInt32:
          return R{sizeof(AvgSt<std::int64_t>), TypeId::kDouble, true, &UpdAvg<std::int64_t, std::int32_t>, &FinAvg<std::int64_t>};
        case TypeId::kInt64:
          return R{sizeof(AvgSt<std::int64_t>), TypeId::kDouble, true, &UpdAvg<std::int64_t, std::int64_t>, &FinAvg<std::int64_t>};
        case TypeId::kDouble:
          return R{sizeof(AvgSt<double>), TypeId::kDouble, true, &UpdAvg<double, double>, &FinAvg<double>};
        default:
          return R{};
      }
  }
  return R{};
}

}  // namespace strata
