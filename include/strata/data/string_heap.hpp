#pragma once

#include <cstddef>
#include <memory>
#include <string_view>
#include <vector>

#include "strata/data/string_ref.hpp"

namespace strata {

// An arena ("bump") allocator for the bytes of VARCHAR values too long to inline
// in a StringRef (> 12 bytes). See ADR 0004.
//
// Invariant that makes StringRef pointers safe: committed bytes NEVER move. We
// only ever append new blocks; existing blocks (and therefore the bytes a
// StringRef points at) stay put for the heap's lifetime. The heap frees all its
// bytes at once on destruction — it cannot free an individual string, which is
// exactly right for batch/columnar processing.
class StringHeap {
 public:
  StringHeap() = default;
  StringHeap(StringHeap&&) noexcept = default;
  StringHeap& operator=(StringHeap&&) noexcept = default;
  StringHeap(const StringHeap&) = delete;
  StringHeap& operator=(const StringHeap&) = delete;

  // Copy `s` into a StringRef. Short strings (<= 12 bytes) are inlined and touch
  // no heap memory; longer strings are copied into the arena.
  StringRef Add(std::string_view s);

  // Total heap bytes handed out (excludes inlined strings). For tests/metrics.
  std::size_t AllocatedBytes() const noexcept { return allocated_; }

 private:
  // Returns a pointer to `n` contiguous, stable bytes in the arena.
  char* Allocate(std::size_t n);

  static constexpr std::size_t kBlockSize = 4096;

  std::vector<std::unique_ptr<char[]>> blocks_;
  char* tail_ = nullptr;            // next free byte in the current block
  std::size_t tail_remaining_ = 0;  // bytes left in the current block
  std::size_t allocated_ = 0;       // total bytes handed out
};

}  // namespace strata
