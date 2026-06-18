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

## Data plane (P1)

The types every operator manipulates, all under `include/strata/data/`:

- **`Vector`** — one column of a batch: an owning, 64-byte-aligned, zero-filled
  data buffer + a `Validity` + (for VARCHAR) a `StringHeap`. Move-only. Kinds:
  `kFlat` (the SIMD fast path) and `kConstant` (one value standing in for all
  rows). Owning buffers (vs DuckDB's shared/ref-counted) is a deliberate P1
  simplicity choice — [ADR 0005](adr/0005-vector-ownership-and-kinds.md).
- **`Validity`** — per-row NULL mask, `uint64` words, **1 = valid**. Unallocated
  until the first NULL, so `AllValid()` is an O(1) fast-path check operators take
  to skip per-row null tests. NULL propagation will compose as a bitwise AND of
  masks — [ADR 0003](adr/0003-validity-mask.md).
- **`StringRef` / `StringHeap`** — the 16-byte German/Umbra string (inline ≤ 12
  bytes, else a 4-byte prefix + pointer into an arena). Equality short-circuits
  on length → prefix → bytes. Why strings blunt SIMD is documented in
  [ADR 0004](adr/0004-string-layout.md) and [`LIMITATIONS.md`](LIMITATIONS.md#3-simd-has-a-real-ceiling-measured-from-p3).
- **`SelectionVector`** — maps logical positions to physical rows so filters/joins
  drop or reorder rows without copying data; identity (no storage) is the fast
  path, and `Compose` cascades selections.
- **`DataChunk`** — a set of equal-length columns (≤ `kVectorSize` = 2048) — the
  unit pushed between operators.

## Module map (fills in by phase)

| Module | Phase | Status |
|--------|-------|--------|
| `common/` (Result, Error, macros) | P0 | done |
| `simd/cpu_features` (preflight) | P0 | done |
| `data/` — `TypeId`, `Validity`, `StringRef`/`StringHeap`, `SelectionVector`, `Vector`, `DataChunk` | P1 | done |
| `ColumnarTable / Scan / Pipeline / Sink` | P2 | — |
| `ExpressionExecutor / Filter / Project` + Highway kernels | P3 | — |
| `HashAggregate` | P4 | — |
| `HashJoin` | P5 | — |
| `Sort / Limit / TopN` | P6 | — |
| `LogicalPlan / Optimizer / Parser` | P7 | — |
| Morsel scheduler (work-stealing) | P8 | — |
| TPC-H harness | P9 | — |
