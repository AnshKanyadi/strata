#include "strata/data/string_heap.hpp"

#include <algorithm>
#include <cstring>

namespace strata {

char* StringHeap::Allocate(std::size_t n) {
  if (n > tail_remaining_) {
    // Start a fresh block. An oversized string gets its own (>= n) block so it
    // is still contiguous; normal allocations use kBlockSize.
    const std::size_t block = std::max(n, kBlockSize);
    blocks_.push_back(std::make_unique<char[]>(block));
    tail_ = blocks_.back().get();
    tail_remaining_ = block;
  }
  char* const p = tail_;
  tail_ += n;
  tail_remaining_ -= n;
  allocated_ += n;
  return p;
}

StringRef StringHeap::Add(std::string_view s) {
  const auto len = static_cast<std::uint32_t>(s.size());
  if (len <= StringRef::kInlineLength) {
    return StringRef::Inlined(s.data(), len);  // no heap touch
  }
  char* const dst = Allocate(len);
  std::memcpy(dst, s.data(), len);
  return StringRef::Pointer(dst, len);
}

}  // namespace strata
