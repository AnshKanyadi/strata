#pragma once

#include <span>

#include "strata/data/types.hpp"

namespace strata {

class DataChunk;  // only referenced by pointer/reference here

// The push-based execution interfaces (ADR 0006), realizing the control-flow
// direction chosen in ADR 0001.
//
//   Source  -- produces batches (GetChunk)
//   Sink    -- consumes batches (Consume) and finalizes (Finalize)
//   Pipeline-- the EXECUTOR: it owns the driving loop and pushes source batches
//              forward into the sink. Operators never hold the loop.

// A Source produces DataChunks. GetChunk() returns the next batch (cardinality
// > 0) or nullptr when exhausted. IMPORTANT: the returned chunk is owned by the
// source (a borrow) and is only valid until the next GetChunk() call — a sink
// that needs to retain the data must copy it out.
class Source {
 public:
  virtual ~Source() = default;
  virtual const DataChunk* GetChunk() = 0;
  virtual std::span<const TypeId> output_types() const = 0;
};

// A Sink is the terminal consumer of a pipeline (a pipeline breaker): an
// aggregate, a join build side, a sort buffer, or — in P2 — a result collector.
class Sink {
 public:
  virtual ~Sink() = default;
  virtual void Consume(const DataChunk& chunk) = 0;
  virtual void Finalize() {}  // called once after the source is exhausted
};

// Pipeline drives one source into one sink. This single-threaded loop is the
// unit that P8 parallelizes: N workers each run this loop over morsels into a
// shared sink whose Finalize() merges their partial state.
class Pipeline {
 public:
  Pipeline(Source& source, Sink& sink) : source_(&source), sink_(&sink) {}

  void Run() {
    while (const DataChunk* chunk = source_->GetChunk()) {
      sink_->Consume(*chunk);
    }
    sink_->Finalize();
  }

 private:
  Source* source_;
  Sink* sink_;
};

}  // namespace strata
