#include "strata/data/data_chunk.hpp"

namespace strata {

void DataChunk::Initialize(std::span<const TypeId> types, std::size_t capacity) {
  capacity_ = capacity;
  size_ = 0;
  columns_.clear();
  columns_.reserve(types.size());
  for (const TypeId t : types) {
    columns_.emplace_back(t, capacity);  // Vector is nothrow-movable, so growth is safe
  }
}

void DataChunk::Reset() noexcept {
  size_ = 0;
  for (Vector& c : columns_) {
    c.validity().Reset();
    c.SetFlat();
  }
}

}  // namespace strata
