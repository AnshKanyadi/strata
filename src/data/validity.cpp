#include "strata/data/validity.hpp"

#include <bit>

namespace strata {

std::size_t Validity::CountValid(std::size_t count) const noexcept {
  if (mask_.empty()) return count;  // all-valid fast path

  std::size_t valid = 0;
  const std::size_t full_words = count >> 6;
  for (std::size_t w = 0; w < full_words; ++w) {
    valid += static_cast<std::size_t>(std::popcount(mask_[w]));
  }
  // Tail: count only the low `rem` bits of the final partial word.
  const std::size_t rem = count & 63;
  if (rem != 0) {
    const std::uint64_t last = mask_[full_words] & ((std::uint64_t{1} << rem) - 1);
    valid += static_cast<std::size_t>(std::popcount(last));
  }
  return valid;
}

}  // namespace strata
