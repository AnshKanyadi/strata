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

## Storage & execution scaffolding (P2)

- **`Schema` / `ColumnarTable`** (`storage/`) — a table is a schema (named, typed
  columns) plus a sequence of `DataChunk`s of ≤ 2048 rows. Storing at execution
  granularity makes `Scan` a zero-copy walk and pre-shapes data for morsels
  (P8). Contrast with DuckDB's 122,880-row row groups in
  [ADR 0007](adr/0007-columnar-storage-and-loader.md).
- **Delimited loader** (`storage/csv_loader`) — a *simple* (non-RFC-4180)
  parser for CSV and TPC-H `.tbl`: configurable delimiter/trailing-delimiter,
  empty-field ⇒ NULL, `std::from_chars` numerics, `YYYY-MM-DD` → int32 epoch-days.
  Errors return via `Result` at the setup boundary.
- **`Source` / `Sink` / `Pipeline`** (`exec/`) — the push model from
  [ADR 0006](adr/0006-pipeline-sink-interfaces.md): a `Source` produces borrowed
  batches (`GetChunk` → `nullptr` at end), a `Sink` consumes them
  (`Consume`/`Finalize`), and the `Pipeline` *is* the driving loop. `Scan` is the
  source; `ResultCollector` is the sink (it deep-copies, since batches are borrowed).

## Expression evaluation & SIMD (P3)

- **SIMD kernels** (`simd/kernels.cc`) — `+ - *` and the six comparisons for
  int32/int64/double, via Google Highway with **runtime dispatch** (NEON on the
  M3, AVX2 on x86 CI from one source). Kernels are value-only (NULL-agnostic);
  dispatch and the op-switch are hoisted **above** the hot loop (once per batch).
  A hand-written scalar reference (`scalar_kernels.hpp`) is the differential-test
  oracle. Rationale + the measured SIMD ceiling: [ADR 0008](adr/0008-simd-kernels-and-the-ceiling.md).
- **`ExpressionExecutor`** (`exec/expression`) — recursively evaluates an
  `Expression` tree (column ref, constant, comparison, arithmetic, AND/OR/NOT)
  to a result `Vector` with **three-valued NULL logic**: strict ops propagate
  NULL as a validity-mask AND; AND/OR/NOT use 3VL truth tables in scalar (a
  decisive `FALSE`/`TRUE` beats a NULL partner). [ADR 0009](adr/0009-vectorized-expression-eval-and-3vl.md).
- **`Filter`** produces a **selection vector** of rows where the predicate is
  TRUE (FALSE and NULL are dropped) — no column copying. **`Project`** evaluates
  its expressions and **gathers** the selected rows into a dense output chunk
  (the materialization point). Full path demonstrated: Scan → Filter → Project →
  ResultCollector.

## Module map (fills in by phase)

| Module | Phase | Status |
|--------|-------|--------|
| `common/` (Result, Error, macros) | P0 | done |
| `simd/cpu_features` (preflight) | P0 | done |
| `data/` — `TypeId`, `Validity`, `StringRef`/`StringHeap`, `SelectionVector`, `Vector`, `DataChunk` | P1 | done |
| `storage/` (`Schema`, `ColumnarTable`, delimited loader) + `exec/` (`Source`/`Sink`/`Pipeline`, `Scan`, `ResultCollector`) | P2 | done |
| `simd/` (Highway kernels + scalar ref) + `exec/` (`ExpressionExecutor`, `Filter`, `Project`) | P3 | done |
| `HashAggregate` | P4 | — |
| `HashJoin` | P5 | — |
| `Sort / Limit / TopN` | P6 | — |
| `LogicalPlan / Optimizer / Parser` | P7 | — |
| Morsel scheduler (work-stealing) | P8 | — |
| TPC-H harness | P9 | — |
