#pragma once

#include <cstddef>
#include <span>
#include <vector>

#include "strata/data/types.hpp"
#include "strata/data/vector.hpp"

namespace strata {

// DataChunk — a horizontal slice of a relation: a set of equal-length columns
// (Vectors) plus a current cardinality `size_` (<= capacity <= kVectorSize).
// This is the unit of data that flows between push-based operators (ADR 0001).
class DataChunk {
 public:
  DataChunk() = default;

  // Allocate one Vector per type, each with the given capacity. Resets size to 0.
  void Initialize(std::span<const TypeId> types, std::size_t capacity = kVectorSize);

  std::size_t ColumnCount() const noexcept { return columns_.size(); }
  std::size_t size() const noexcept { return size_; }            // current cardinality
  void SetSize(std::size_t n) noexcept { size_ = n; }
  std::size_t capacity() const noexcept { return capacity_; }

  Vector& column(std::size_t i) noexcept { return columns_[i]; }
  const Vector& column(std::size_t i) const noexcept { return columns_[i]; }

  // Reuse the chunk for a new batch: clear cardinality and reset each column to
  // the all-valid, flat fast path (keeps the allocated buffers).
  void Reset() noexcept;

 private:
  std::vector<Vector> columns_;
  std::size_t size_ = 0;
  std::size_t capacity_ = 0;
};

}  // namespace strata
