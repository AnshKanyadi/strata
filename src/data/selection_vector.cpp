#include "strata/data/selection_vector.hpp"

namespace strata {

SelectionVector SelectionVector::Slice(std::size_t offset, std::size_t count) const {
  SelectionVector out(count);
  for (std::size_t j = 0; j < count; ++j) {
    out.Set(j, Get(offset + j));
  }
  return out;
}

SelectionVector SelectionVector::Compose(const SelectionVector& inner,
                                         std::size_t count) const {
  SelectionVector out(count);
  for (std::size_t j = 0; j < count; ++j) {
    // r[j] = this[inner[j]] — cascade an inner selection through this one.
    out.Set(j, Get(inner.Get(j)));
  }
  return out;
}

}  // namespace strata
