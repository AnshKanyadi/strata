#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <string_view>

#include "strata/config.hpp"  // kVectorSize
#include "strata/data/string_heap.hpp"
#include "strata/data/string_ref.hpp"
#include "strata/data/types.hpp"
#include "strata/data/validity.hpp"

namespace strata {

// AlignedBuffer — a move-only, RAII, 64-byte-aligned, zero-initialized byte
// buffer. 64 bytes covers NEON (16), AVX2 (32) and AVX-512 (64) aligned loads in
// one constant and avoids split cache-line accesses in the P3 kernels (ADR 0005).
class AlignedBuffer {
 public:
  static constexpr std::size_t kAlignment = 64;

  AlignedBuffer() = default;
  explicit AlignedBuffer(std::size_t bytes);
  ~AlignedBuffer();

  AlignedBuffer(AlignedBuffer&& other) noexcept;
  AlignedBuffer& operator=(AlignedBuffer&& other) noexcept;
  AlignedBuffer(const AlignedBuffer&) = delete;
  AlignedBuffer& operator=(const AlignedBuffer&) = delete;

  std::byte* data() noexcept { return ptr_; }
  const std::byte* data() const noexcept { return ptr_; }
  std::size_t size() const noexcept { return size_; }

 private:
  std::byte* ptr_ = nullptr;
  std::size_t size_ = 0;
};

enum class VectorKind : std::uint8_t {
  kFlat,      // a contiguous array of `count` values (the SIMD fast path)
  kConstant,  // one logical value at slot 0 standing in for every row
};

// Vector — one column's worth of a batch: a typed data buffer, a Validity mask,
// and (for VARCHAR) a StringHeap. Owns its storage and is MOVE-ONLY so columns
// never get deep-copied by accident. See ADR 0005.
class Vector {
 public:
  explicit Vector(TypeId type, std::size_t capacity = kVectorSize);

  Vector(Vector&&) noexcept = default;
  Vector& operator=(Vector&&) noexcept = default;
  Vector(const Vector&) = delete;
  Vector& operator=(const Vector&) = delete;

  // Build a constant vector (capacity 1; readers use slot 0 for every row).
  static Vector Constant(TypeId type);

  TypeId type() const noexcept { return type_; }
  VectorKind kind() const noexcept { return kind_; }
  std::size_t capacity() const noexcept { return capacity_; }

  void SetConstant() noexcept { kind_ = VectorKind::kConstant; }
  void SetFlat() noexcept { kind_ = VectorKind::kFlat; }
  bool IsConstant() const noexcept { return kind_ == VectorKind::kConstant; }

  // Typed view over the data buffer. The buffer is over-aligned, so these loads
  // are well-aligned for T; for trivially-copyable T, C++20 implicit object
  // creation makes this defined as long as a slot is written before it is read
  // (the buffer is zero-initialized, so an unwritten slot reads as a zero value).
  template <class T>
  T* Data() noexcept {
    return reinterpret_cast<T*>(buffer_.data());
  }
  template <class T>
  const T* Data() const noexcept {
    return reinterpret_cast<const T*>(buffer_.data());
  }

  Validity& validity() noexcept { return validity_; }
  const Validity& validity() const noexcept { return validity_; }

  // --- convenience helpers (test/loader paths, not the per-batch hot loop) ---
  template <class T>
  void Set(std::size_t idx, T value) noexcept {
    Data<T>()[idx] = value;
  }
  template <class T>
  T Get(std::size_t idx) const noexcept {
    return Data<T>()[idx];
  }
  void SetNull(std::size_t idx) { validity_.SetInvalid(idx); }
  bool IsNull(std::size_t idx) const noexcept { return !validity_.RowIsValid(idx); }

  // --- VARCHAR ---
  // Copy `s` into this vector's heap and return the StringRef to store via
  // Set<StringRef>/Data<StringRef>(). Requires type() == kVarchar.
  StringRef AddString(std::string_view s);
  StringHeap& string_heap() noexcept { return *heap_; }

  // --- constant-vector accessors ---
  template <class T>
  T ConstantValue() const noexcept {
    return Data<T>()[0];
  }
  bool ConstantIsNull() const noexcept { return !validity_.RowIsValid(0); }

 private:
  TypeId type_;
  VectorKind kind_ = VectorKind::kFlat;
  std::size_t capacity_;
  AlignedBuffer buffer_;
  Validity validity_;
  std::unique_ptr<StringHeap> heap_;  // non-null iff type_ == kVarchar
};

}  // namespace strata
