#pragma once

#include <cstddef>
#include <cstdint>

namespace strata::simd {

enum class ArithOp : std::uint8_t { kAdd, kSub, kMul };
enum class CmpOp : std::uint8_t { kEq, kNe, kLt, kLe, kGt, kGe };

// Dispatched SIMD kernels (Google Highway, runtime dispatch — NEON on arm64,
// AVX2 on x86; the CPU target is chosen once at startup). See ADR 0008.
//
// Contract: these operate on FLAT arrays of `n` values and know NOTHING about
// NULLs — three-valued NULL handling is the caller's job (validity-mask
// combining). The op is selected by a single switch per call (per batch), so
// the inner loops are branch-free. Comparison emits uint8 0/1 per row.
// Integer arithmetic WRAPS on overflow (two's complement); see ADR 0008.
void Arith(ArithOp op, const std::int32_t* a, const std::int32_t* b, std::int32_t* out, std::size_t n);
void Arith(ArithOp op, const std::int64_t* a, const std::int64_t* b, std::int64_t* out, std::size_t n);
void Arith(ArithOp op, const double* a, const double* b, double* out, std::size_t n);

void Compare(CmpOp op, const std::int32_t* a, const std::int32_t* b, std::uint8_t* out, std::size_t n);
void Compare(CmpOp op, const std::int64_t* a, const std::int64_t* b, std::uint8_t* out, std::size_t n);
void Compare(CmpOp op, const double* a, const double* b, std::uint8_t* out, std::size_t n);

}  // namespace strata::simd
