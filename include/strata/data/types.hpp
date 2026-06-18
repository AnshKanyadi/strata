#pragma once

#include <cstddef>
#include <cstdint>

namespace strata {

// The fixed starter type set (see DESIGN). DATE is physically an int32 day
// count; BOOL is one byte (0/1), not bit-packed, so it stays SIMD-addressable;
// VARCHAR values are 16-byte StringRefs (see string_ref.hpp / ADR 0004).
// DECIMAL is added later only if a target query needs it.
enum class TypeId : std::uint8_t {
  kBool,
  kInt32,
  kInt64,
  kDouble,
  kDate,
  kVarchar,
};

// Physical size, in bytes, of one stored value of this type — i.e. the stride
// of a flat Vector's data buffer. (kVarchar == sizeof(StringRef), asserted in
// string_ref.hpp so the two cannot silently drift apart.)
constexpr std::size_t PhysicalSize(TypeId type) noexcept {
  switch (type) {
    case TypeId::kBool:    return 1;   // uint8 0/1
    case TypeId::kInt32:   return 4;
    case TypeId::kInt64:   return 8;
    case TypeId::kDouble:  return 8;
    case TypeId::kDate:    return 4;   // int32 days since 1970-01-01
    case TypeId::kVarchar: return 16;  // StringRef
  }
  return 0;  // unreachable for valid enum values
}

// Stable human-readable name (defined in types.cpp).
const char* TypeName(TypeId type) noexcept;

}  // namespace strata
