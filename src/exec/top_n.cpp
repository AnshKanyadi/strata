#include "strata/exec/top_n.hpp"

#include <algorithm>
#include <numeric>
#include <utility>

#include "strata/data/column_ops.hpp"

namespace strata {

TopN::TopN(std::vector<SortKey> keys, std::size_t k, Sink& output)
    : keys_(std::move(keys)), k_(k), output_(&output) {}

void TopN::Consume(const DataChunk& src) {
  if (k_ == 0) return;
  if (types_.empty()) {
    for (std::size_t c = 0; c < src.ColumnCount(); ++c) types_.push_back(src.column(c).type());
    kept_.Initialize(types_, k_);
    heap_.reserve(k_);
  }

  // Max-heap comparator: x "less than" y means x sorts BEFORE y (x is better);
  // std::*_heap then keeps the GREATEST (the worst row) at the root.
  const auto worse = [&](std::uint32_t a, std::uint32_t b) {
    return CompareRows(kept_, a, kept_, b, keys_) < 0;
  };

  const std::size_t n = src.size();
  for (std::size_t i = 0; i < n; ++i) {
    auto write_row = [&](std::uint32_t slot) {
      for (std::size_t c = 0; c < types_.size(); ++c) {
        CopyElement(src.column(c), i, kept_.column(c), slot);
      }
    };
    if (count_ < k_) {
      const std::uint32_t slot = static_cast<std::uint32_t>(count_++);
      write_row(slot);
      heap_.push_back(slot);
      std::push_heap(heap_.begin(), heap_.end(), worse);
    } else if (CompareRows(src, i, kept_, heap_.front(), keys_) < 0) {
      // Better than the current worst: evict the root and reuse its slot.
      std::pop_heap(heap_.begin(), heap_.end(), worse);
      const std::uint32_t slot = heap_.back();
      write_row(slot);
      std::push_heap(heap_.begin(), heap_.end(), worse);
    }
  }
}

void TopN::Finalize() {
  std::vector<std::uint32_t> order(count_);
  std::iota(order.begin(), order.end(), std::uint32_t{0});  // kept rows occupy slots [0, count_)
  std::stable_sort(order.begin(), order.end(), [&](std::uint32_t a, std::uint32_t b) {
    return CompareRows(kept_, a, kept_, b, keys_) < 0;
  });

  DataChunk out;
  std::size_t produced = 0;
  while (produced < count_) {
    const std::size_t batch = std::min(kVectorSize, count_ - produced);
    out.Initialize(types_, kVectorSize);
    for (std::size_t j = 0; j < batch; ++j) {
      const std::uint32_t slot = order[produced + j];
      for (std::size_t c = 0; c < types_.size(); ++c) {
        CopyElement(kept_.column(c), slot, out.column(c), j);
      }
    }
    out.SetSize(batch);
    output_->Consume(out);
    produced += batch;
  }
  output_->Finalize();
}

}  // namespace strata
