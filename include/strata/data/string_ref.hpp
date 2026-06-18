#pragma once

#include <cstdint>
#include <cstring>
#include <string_view>
#include <type_traits>

namespace strata {

// StringRef — the 16-byte "German string" (Umbra; also DuckDB's string_t).
// See ADR 0004 for the full rationale and the SIMD-ceiling discussion.
//
// Layout (16 bytes total):
//   - a 4-byte length, then a 12-byte union of two arms that SHARE that length
//     as their common initial member:
//       inlined arm : { uint32 length; char  inlined[12]; }   // len <= 12
//       pointer arm : { uint32 length; char  prefix[4]; char* ptr; }  // len > 12
//   - Strings up to 12 bytes live entirely inside the StringRef — no heap, no
//     dereference. Longer strings keep a 4-byte prefix inline (so equality and
//     ordering can reject most mismatches without chasing the pointer) and point
//     `ptr` at the full bytes in a StringHeap.
//   - 12 is the inline cutoff because the pointer arm spends prefix(4)+ptr(8)=12
//     bytes anyway; inlining up to 12 is "free" relative to that.
class StringRef {
 public:
  static constexpr std::uint32_t kPrefixLength = 4;
  static constexpr std::uint32_t kInlineLength = 12;

  // Zero-fills the storage so unused inline bytes are deterministic (equality
  // and hashing can then read the whole prefix word safely).
  StringRef() noexcept { std::memset(&u_, 0, sizeof(u_)); }

  // Build an inlined StringRef. Precondition: len <= kInlineLength.
  static StringRef Inlined(const char* bytes, std::uint32_t len) noexcept {
    StringRef s;  // zero-filled
    s.u_.inlined.length = len;
    if (len != 0) std::memcpy(s.u_.inlined.inlined, bytes, len);
    return s;
  }

  // Build a pointer StringRef. `heap_ptr` must point at `len` bytes that outlive
  // this StringRef. Precondition: len > kInlineLength (so >= 4 prefix bytes exist).
  static StringRef Pointer(const char* heap_ptr, std::uint32_t len) noexcept {
    StringRef s;  // zero-filled
    s.u_.pointer.length = len;
    std::memcpy(s.u_.pointer.prefix, heap_ptr, kPrefixLength);
    s.u_.pointer.ptr = heap_ptr;
    return s;
  }

  // `length` is the common initial sequence of both union arms, so reading it
  // through either arm is well-defined regardless of which arm is active.
  std::uint32_t size() const noexcept { return u_.inlined.length; }
  bool empty() const noexcept { return size() == 0; }
  bool IsInlined() const noexcept { return size() <= kInlineLength; }

  const char* data() const noexcept {
    return IsInlined() ? u_.inlined.inlined : u_.pointer.ptr;
  }
  std::string_view view() const noexcept { return {data(), size()}; }

  // Equality with the German-string short-circuits: length, then the 4-byte
  // prefix (no heap touch), then the full bytes only if still ambiguous.
  friend bool operator==(const StringRef& a, const StringRef& b) noexcept {
    if (a.size() != b.size()) return false;
    if (std::memcmp(a.Prefix(), b.Prefix(), kPrefixLength) != 0) return false;
    if (a.size() <= kPrefixLength) return true;  // prefix covered every byte
    return std::memcmp(a.data(), b.data(), a.size()) == 0;
  }
  friend bool operator!=(const StringRef& a, const StringRef& b) noexcept {
    return !(a == b);
  }

 private:
  // The active arm's prefix bytes (inlined storage when inlined, the cached
  // prefix when pointed-to). Always reads the active member.
  const char* Prefix() const noexcept {
    return IsInlined() ? u_.inlined.inlined : u_.pointer.prefix;
  }

  union U {
    struct Pointer {
      std::uint32_t length;
      char prefix[kPrefixLength];
      const char* ptr;
    } pointer;
    struct Inlined {
      std::uint32_t length;
      char inlined[kInlineLength];
    } inlined;
  } u_;
};

static_assert(sizeof(StringRef) == 16, "StringRef must be the 16-byte German-string layout");
static_assert(alignof(StringRef) == 8);
static_assert(std::is_trivially_copyable_v<StringRef>,
              "StringRef must be trivially copyable to live in raw Vector buffers");

}  // namespace strata
