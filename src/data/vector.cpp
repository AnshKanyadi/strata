#include "strata/data/vector.hpp"

#include <cassert>
#include <cstring>

namespace strata {

// --- AlignedBuffer ----------------------------------------------------------

AlignedBuffer::AlignedBuffer(std::size_t bytes) {
  if (bytes == 0) return;
  // Round the request up to the alignment so the byte count is a clean multiple
  // (tidy for SIMD tail handling) and the matching aligned delete is unambiguous.
  const std::size_t rounded = ((bytes + kAlignment - 1) / kAlignment) * kAlignment;
  ptr_ = static_cast<std::byte*>(::operator new(rounded, std::align_val_t{kAlignment}));
  std::memset(ptr_, 0, rounded);  // zero-init: an unwritten slot reads as zero
  size_ = rounded;
}

AlignedBuffer::~AlignedBuffer() {
  if (ptr_ != nullptr) {
    // Sized, aligned delete: we pass the same byte count we allocated, letting
    // the allocator skip a size lookup. (`size_` is the rounded request.)
    ::operator delete(ptr_, size_, std::align_val_t{kAlignment});
  }
}

AlignedBuffer::AlignedBuffer(AlignedBuffer&& other) noexcept
    : ptr_(other.ptr_), size_(other.size_) {
  other.ptr_ = nullptr;
  other.size_ = 0;
}

AlignedBuffer& AlignedBuffer::operator=(AlignedBuffer&& other) noexcept {
  if (this != &other) {
    if (ptr_ != nullptr) {
      // Sized, aligned delete: we pass the same byte count we allocated, letting
    // the allocator skip a size lookup. (`size_` is the rounded request.)
    ::operator delete(ptr_, size_, std::align_val_t{kAlignment});
    }
    ptr_ = other.ptr_;
    size_ = other.size_;
    other.ptr_ = nullptr;
    other.size_ = 0;
  }
  return *this;
}

// --- Vector -----------------------------------------------------------------

Vector::Vector(TypeId type, std::size_t capacity)
    : type_(type),
      capacity_(capacity),
      buffer_(capacity * PhysicalSize(type)),
      validity_(capacity) {
  if (type == TypeId::kVarchar) {
    heap_ = std::make_unique<StringHeap>();
  }
}

Vector Vector::Constant(TypeId type) {
  Vector v(type, 1);
  v.SetConstant();
  return v;
}

StringRef Vector::AddString(std::string_view s) {
  assert(heap_ != nullptr && "AddString requires a VARCHAR vector");
  return heap_->Add(s);
}

}  // namespace strata
