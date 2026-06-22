#pragma once

#include <cstddef>

#include "strata/data/data_chunk.hpp"
#include "strata/exec/pipeline.hpp"

namespace strata {

// Limit — a streaming Sink that forwards the first n rows downstream and drops
// the rest. NOTE: early termination (signalling the source to stop scanning) is
// the deferred push "stop" signal from ADR 0001/0006 — so a bare LIMIT still
// consumes all input. Correct, not optimal. See ADR 0013.
class Limit final : public Sink {
 public:
  Limit(std::size_t n, Sink& output) : n_(n), output_(&output) {}
  void Consume(const DataChunk& chunk) override;
  void Finalize() override { output_->Finalize(); }

 private:
  std::size_t n_;
  std::size_t emitted_ = 0;
  Sink* output_;
};

}  // namespace strata
