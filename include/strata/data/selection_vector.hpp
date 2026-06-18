#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace strata {

using sel_t = std::uint32_t;

// SelectionVector — maps logical positions [0, count) to physical row indices in
// a Vector. This is how Strata filters/joins drop or reorder rows WITHOUT
// copying column data: an operator narrows the set of live rows by producing a
// selection vector that downstream operators honor (see DESIGN; the discipline
// is foreshadowed for the Filter/Project work in P3).
//
// Fast path: an "identity" selection has no backing storage and maps i -> i.
// A freshly default-constructed SelectionVector is the identity. Cascading two
// selections (Compose) lets a filter refine an already-selected set cheaply.
class SelectionVector {
 public:
  SelectionVector() = default;  // identity
  explicit SelectionVector(std::size_t capacity) : sel_(capacity, 0) {}

  bool IsIdentity() const noexcept { return sel_.empty(); }

  sel_t Get(std::size_t i) const noexcept {
    return sel_.empty() ? static_cast<sel_t>(i) : sel_[i];
  }
  // Requires owned (non-identity) storage; precondition i < capacity().
  void Set(std::size_t i, sel_t value) noexcept { sel_[i] = value; }

  sel_t* data() noexcept { return sel_.data(); }
  const sel_t* data() const noexcept { return sel_.empty() ? nullptr : sel_.data(); }
  std::size_t capacity() const noexcept { return sel_.size(); }

  void InitOwned(std::size_t capacity) { sel_.assign(capacity, 0); }

  // Returns an owned selection r with r.Get(j) == this->Get(offset + j) for
  // j in [0, count). Slicing the identity by (offset, count) yields offset+j.
  SelectionVector Slice(std::size_t offset, std::size_t count) const;

  // Cascade: returns an owned selection r with r.Get(j) == this->Get(inner.Get(j))
  // for j in [0, count). This is how a second filter refines the first's result
  // without ever materializing intermediate rows.
  SelectionVector Compose(const SelectionVector& inner, std::size_t count) const;

 private:
  std::vector<sel_t> sel_;  // empty == identity
};

}  // namespace strata
