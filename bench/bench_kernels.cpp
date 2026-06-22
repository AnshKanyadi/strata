// Scalar vs. SIMD microbenchmarks for the P3 kernels (ADR 0008). Numbers and
// the honest interpretation live in docs/BENCHMARKS.md.
//
// The scalar baselines disable auto-vectorization (clang pragma) so the result
// isolates the SIMD contribution. IMPORTANT CAVEAT (recorded in BENCHMARKS.md):
// at -O3 the compiler would otherwise auto-vectorize these simple loops, so the
// speedup shown here is "Highway SIMD vs. genuinely-scalar", an upper bound on
// the gain over compiler-auto-vectorized scalar. On GCC the pragma is absent
// (guarded), so the GCC scalar baseline may auto-vectorize.

#include <benchmark/benchmark.h>

#include <cstddef>
#include <cstdint>
#include <random>
#include <vector>

#include "strata/config.hpp"
#include "strata/simd/kernels.hpp"

namespace {

constexpr std::size_t N = strata::kVectorSize;  // one batch (2048)

template <class T>
std::vector<T> MakeData(std::uint64_t seed) {
  std::mt19937_64 g(seed);
  std::vector<T> v(N);
  for (auto& x : v) x = static_cast<T>(g() % 1000 + 1);  // small => no overflow
  return v;
}

#if defined(__clang__)
#define STRATA_NO_VECTORIZE _Pragma("clang loop vectorize(disable) interleave(disable)")
#else
#define STRATA_NO_VECTORIZE
#endif

template <class T>
[[gnu::noinline]] void ScalarAdd(const T* a, const T* b, T* o, std::size_t n) {
  STRATA_NO_VECTORIZE
  for (std::size_t i = 0; i < n; ++i) o[i] = a[i] + b[i];
}
template <class T>
[[gnu::noinline]] void ScalarMul(const T* a, const T* b, T* o, std::size_t n) {
  STRATA_NO_VECTORIZE
  for (std::size_t i = 0; i < n; ++i) o[i] = a[i] * b[i];
}
template <class T>
[[gnu::noinline]] void ScalarGt(const T* a, const T* b, std::uint8_t* o, std::size_t n) {
  STRATA_NO_VECTORIZE
  for (std::size_t i = 0; i < n; ++i) o[i] = a[i] > b[i] ? std::uint8_t{1} : std::uint8_t{0};
}

void SetThroughput(benchmark::State& s) {
  s.SetItemsProcessed(static_cast<std::int64_t>(s.iterations()) * static_cast<std::int64_t>(N));
}

}  // namespace

// --- int32 add ---
static void BM_Add_i32_scalar(benchmark::State& s) {
  const auto a = MakeData<std::int32_t>(1), b = MakeData<std::int32_t>(2);
  std::vector<std::int32_t> o(N);
  for (auto _ : s) { ScalarAdd(a.data(), b.data(), o.data(), N); benchmark::DoNotOptimize(o.data()); benchmark::ClobberMemory(); }
  SetThroughput(s);
}
static void BM_Add_i32_simd(benchmark::State& s) {
  const auto a = MakeData<std::int32_t>(1), b = MakeData<std::int32_t>(2);
  std::vector<std::int32_t> o(N);
  for (auto _ : s) { strata::simd::Arith(strata::simd::ArithOp::kAdd, a.data(), b.data(), o.data(), N); benchmark::DoNotOptimize(o.data()); benchmark::ClobberMemory(); }
  SetThroughput(s);
}
BENCHMARK(BM_Add_i32_scalar);
BENCHMARK(BM_Add_i32_simd);

// --- int32 multiply ---
static void BM_Mul_i32_scalar(benchmark::State& s) {
  const auto a = MakeData<std::int32_t>(3), b = MakeData<std::int32_t>(4);
  std::vector<std::int32_t> o(N);
  for (auto _ : s) { ScalarMul(a.data(), b.data(), o.data(), N); benchmark::DoNotOptimize(o.data()); benchmark::ClobberMemory(); }
  SetThroughput(s);
}
static void BM_Mul_i32_simd(benchmark::State& s) {
  const auto a = MakeData<std::int32_t>(3), b = MakeData<std::int32_t>(4);
  std::vector<std::int32_t> o(N);
  for (auto _ : s) { strata::simd::Arith(strata::simd::ArithOp::kMul, a.data(), b.data(), o.data(), N); benchmark::DoNotOptimize(o.data()); benchmark::ClobberMemory(); }
  SetThroughput(s);
}
BENCHMARK(BM_Mul_i32_scalar);
BENCHMARK(BM_Mul_i32_simd);

// --- double add ---
static void BM_Add_f64_scalar(benchmark::State& s) {
  const auto a = MakeData<double>(5), b = MakeData<double>(6);
  std::vector<double> o(N);
  for (auto _ : s) { ScalarAdd(a.data(), b.data(), o.data(), N); benchmark::DoNotOptimize(o.data()); benchmark::ClobberMemory(); }
  SetThroughput(s);
}
static void BM_Add_f64_simd(benchmark::State& s) {
  const auto a = MakeData<double>(5), b = MakeData<double>(6);
  std::vector<double> o(N);
  for (auto _ : s) { strata::simd::Arith(strata::simd::ArithOp::kAdd, a.data(), b.data(), o.data(), N); benchmark::DoNotOptimize(o.data()); benchmark::ClobberMemory(); }
  SetThroughput(s);
}
BENCHMARK(BM_Add_f64_scalar);
BENCHMARK(BM_Add_f64_simd);

// --- int32 compare (greater-than -> uint8) ---
static void BM_Gt_i32_scalar(benchmark::State& s) {
  const auto a = MakeData<std::int32_t>(7), b = MakeData<std::int32_t>(8);
  std::vector<std::uint8_t> o(N);
  for (auto _ : s) { ScalarGt(a.data(), b.data(), o.data(), N); benchmark::DoNotOptimize(o.data()); benchmark::ClobberMemory(); }
  SetThroughput(s);
}
static void BM_Gt_i32_simd(benchmark::State& s) {
  const auto a = MakeData<std::int32_t>(7), b = MakeData<std::int32_t>(8);
  std::vector<std::uint8_t> o(N);
  for (auto _ : s) { strata::simd::Compare(strata::simd::CmpOp::kGt, a.data(), b.data(), o.data(), N); benchmark::DoNotOptimize(o.data()); benchmark::ClobberMemory(); }
  SetThroughput(s);
}
BENCHMARK(BM_Gt_i32_scalar);
BENCHMARK(BM_Gt_i32_simd);

// --- double compare ---
static void BM_Gt_f64_scalar(benchmark::State& s) {
  const auto a = MakeData<double>(9), b = MakeData<double>(10);
  std::vector<std::uint8_t> o(N);
  for (auto _ : s) { ScalarGt(a.data(), b.data(), o.data(), N); benchmark::DoNotOptimize(o.data()); benchmark::ClobberMemory(); }
  SetThroughput(s);
}
static void BM_Gt_f64_simd(benchmark::State& s) {
  const auto a = MakeData<double>(9), b = MakeData<double>(10);
  std::vector<std::uint8_t> o(N);
  for (auto _ : s) { strata::simd::Compare(strata::simd::CmpOp::kGt, a.data(), b.data(), o.data(), N); benchmark::DoNotOptimize(o.data()); benchmark::ClobberMemory(); }
  SetThroughput(s);
}
BENCHMARK(BM_Gt_f64_scalar);
BENCHMARK(BM_Gt_f64_simd);

BENCHMARK_MAIN();
