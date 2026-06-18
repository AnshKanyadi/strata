#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace strata {

// Validity — per-value NULL tracking for one Vector. See ADR 0003.
//
// Convention: bit set (1) == VALID (not null), matching DuckDB. The mask is a
// flat array of uint64 words; value i lives in word (i >> 6), bit (i & 63).
//
// THE fast path: while a column has no NULLs the mask stays UNALLOCATED, and
// AllValid() is an O(1) "is the buffer empty?" check. Operators test AllValid()
// once per batch and then skip all per-row null checks — the common case for
// analytical columns. The mask is allocated (initialized all-ones = all valid)
// lazily, the first time a NULL is set.
//
// Three-valued-logic foreshadow (P3): because 1==valid, combining the validity
// of two inputs is a bitwise AND of their masks — a result is non-null only when
// every input is non-null.
class Validity {
 public:
  Validity() = default;
  explicit Validity(std::size_t capacity) noexcept : capacity_(capacity) {}

  // True iff no NULL has ever been recorded (no mask allocated). Note: we do not
  // re-tighten to true if bits are individually set valid again after allocation.
  bool AllValid() const noexcept { return mask_.empty(); }

  bool RowIsValid(std::size_t idx) const noexcept {
    if (mask_.empty()) return true;
    return ((mask_[idx >> 6] >> (idx & 63)) & std::uint64_t{1}) != 0;
  }

  // Precondition for both: idx < capacity().
  void SetInvalid(std::size_t idx) {
    EnsureAllocated();
    mask_[idx >> 6] &= ~(std::uint64_t{1} << (idx & 63));
  }
  void SetValid(std::size_t idx) {
    EnsureAllocated();
    mask_[idx >> 6] |= (std::uint64_t{1} << (idx & 63));
  }

  // Reset back to the all-valid fast path (drops the mask allocation).
  void Reset() noexcept { mask_.clear(); }
  void Reset(std::size_t capacity) noexcept {
    mask_.clear();
    capacity_ = capacity;
  }

  // Number of valid (non-null) values among the first `count`. Precondition:
  // count <= capacity(). Uses std::popcount; masks off bits beyond `count`.
  std::size_t CountValid(std::size_t count) const noexcept;

  // Raw words, or nullptr when all-valid. (For SIMD mask combining later.)
  const std::uint64_t* data() const noexcept {
    return mask_.empty() ? nullptr : mask_.data();
  }
  std::size_t capacity() const noexcept { return capacity_; }

  static constexpr std::size_t WordCount(std::size_t n) noexcept { return (n + 63) / 64; }

 private:
  void EnsureAllocated() {
    if (mask_.empty()) {
      mask_.assign(WordCount(capacity_), ~std::uint64_t{0});  // all bits valid
    }
  }

  std::vector<std::uint64_t> mask_;
  std::size_t capacity_ = 0;
};

}  // namespace strata
