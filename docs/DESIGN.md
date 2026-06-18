# Strata — Design

> Living document. P0 records the foundations; each phase appends its module.
> The deep rationale for individual decisions lives in [`adr/`](adr/); this file
> is the connective tissue between them.

## North star

Vectorized execution over in-memory columnar data: operators consume and produce
`DataChunk`s of up to `VECTOR_SIZE = 2048` values, so per-call overhead is
amortized across the batch and the working set stays cache-resident (the
MonetDB/X100 insight). See `include/strata/config.hpp` for the cache reasoning
behind 2048.

## Execution model: push-based pipelines

Sources push `DataChunk`s through a chain of operators into a **sink** (hash
aggregate, join build, sort buffer, or result collector). This is chosen over
the pull-based Volcano iterator model primarily because morsel-driven
parallelism (P8) composes naturally with push. Full rationale and tradeoffs:
[ADR 0001](adr/0001-push-based-execution.md).

## Toolchain & error handling

C++23 with Homebrew LLVM/libc++; `Result<T> = std::expected<T, Error>` for
allocation-free, exception-free error propagation in the hot path (exceptions
allowed only at the query/setup boundary). Rationale and the dependency-pinning
strategy: [ADR 0002](adr/0002-cxx23-homebrew-llvm-toolchain.md).

## Vectorization vs. compilation

Strata vectorizes; it does not JIT-compile. The two are broadly competitive
(Kersten et al., VLDB 2018); we trade a compiler's pipeline-fusion for
debuggability, predictable latency, and no codegen surface. Expanded in
[`LIMITATIONS.md`](LIMITATIONS.md#1-vectorization-not-compilation).

## Module map (fills in by phase)

| Module | Phase | Status |
|--------|-------|--------|
| `common/` (Result, Error, macros) | P0 | done |
| `simd/cpu_features` (preflight) | P0 | done |
| `Vector / Validity / DataChunk / SelectionVector / StringHeap` | P1 | — |
| `ColumnarTable / Scan / Pipeline / Sink` | P2 | — |
| `ExpressionExecutor / Filter / Project` + Highway kernels | P3 | — |
| `HashAggregate` | P4 | — |
| `HashJoin` | P5 | — |
| `Sort / Limit / TopN` | P6 | — |
| `LogicalPlan / Optimizer / Parser` | P7 | — |
| Morsel scheduler (work-stealing) | P8 | — |
| TPC-H harness | P9 | — |
