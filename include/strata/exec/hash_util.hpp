#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "strata/data/string_ref.hpp"
#include "strata/data/types.hpp"
#include "strata/data/vector.hpp"

// Shared hashing for the keyed hash tables (aggregate GROUP BY and join). Using
// one mixer on both sides of a join is what makes equal keys land in the same
// slot. Inline (header-only) so both translation units share one definition.
namespace strata::hashing {

// murmur3 64-bit finalizer — spreads bits so the low bits used for the slot
// index are well-mixed.
inline std::uint64_t Mix(std::uint64_t x) noexcept {
  x ^= x >> 33;
  x *= 0xff51afd7ed558ccdULL;
  x ^= x >> 33;
  x *= 0xc4ceb9fe1a85ec53ULL;
  x ^= x >> 33;
  return x;
}

inline std::uint64_t Combine(std::uint64_t seed, std::uint64_t h) noexcept {
  return seed ^ (h + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2));
}

inline std::uint64_t FnvHash(std::string_view s) noexcept {
  std::uint64_t h = 0xcbf29ce484222325ULL;
  for (const char c : s) {
    h ^= static_cast<std::uint64_t>(static_cast<std::uint8_t>(c));
    h *= 0x100000001b3ULL;
  }
  return h;
}

// Hash one (valid, non-NULL) value of the given type at row i of `col`.
inline std::uint64_t HashValue(TypeId t, const Vector& col, std::size_t i) {
  switch (t) {
    case TypeId::kInt32:
    case TypeId::kDate:
      return Mix(static_cast<std::uint64_t>(static_cast<std::uint32_t>(col.Get<std::int32_t>(i))));
    case TypeId::kInt64:
      return Mix(static_cast<std::uint64_t>(col.Get<std::int64_t>(i)));
    case TypeId::kDouble:
      return Mix(std::bit_cast<std::uint64_t>(col.Get<double>(i)));
    case TypeId::kBool:
      return Mix(col.Get<std::uint8_t>(i));
    case TypeId::kVarchar:
      return Mix(FnvHash(col.Get<StringRef>(i).view()));
  }
  return 0;
}

inline constexpr std::uint64_t kFnvSeed = 0xcbf29ce484222325ULL;

}  // namespace strata::hashing
