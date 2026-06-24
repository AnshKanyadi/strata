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

## Hash aggregation (P4)

`HashAggregate` is a `Sink` implementing `GROUP BY` and global aggregation
(`COUNT(*)`, `COUNT`, `SUM`, `MIN`, `MAX`, `AVG`):

- **Open-addressing, linear-probe table** ([ADR 0010](adr/0010-aggregate-hash-table.md)).
  Group rows live in one contiguous buffer, each `[ hash | key values |
  key null-flags | aggregate states ]`; a slots array holds `{group index, salt}`.
  Probe rejects on the 1-byte salt before any full key compare; grows by doubling
  + re-slotting via the stored hash. Key + state share one row → one cache-line
  region per probe, no per-group allocation (vs `std::unordered_map`).
- **NULL is its own group** (null-flag in the key; `NULL == NULL` for grouping);
  global aggregation is a single pre-created group.
- **Per-batch scatter-update** ([ADR 0011](adr/0011-aggregate-state-and-overflow.md)):
  probe the whole batch → `gidx[]`, then each aggregate updates its inline state
  once per batch. The scatter is random-access (gather/scatter) — memory-bound,
  not a SIMD target (consistent with ADR 0008's ceiling).
- **NULL + overflow**: aggregates skip NULLs (except `COUNT(*)`); empty/all-NULL
  groups yield NULL for SUM/MIN/MAX/AVG and 0 for COUNT. `SUM(int32)` widens to an
  int64 accumulator (overflow-safe); we keep int64 (not `__int128`) for
  `-Wpedantic` portability across the clang+gcc CI matrix — DuckDB uses int128
  (values cross-checked, only the output type differs).

## Hash join (P5)

`HashJoinBuild` / `HashJoinProbe` implement an inner equi-join ([ADR 0012](adr/0012-hash-join.md)):

- **Chained** build table (vs the aggregate table's open addressing) because joins
  retain *every* build row and keys repeat: a slots array points to a chain head,
  and each build row carries an in-buffer `next` index (no per-node allocation).
- **Row-layout** build rows `[ hash | column values | null-flags | next ]` — fully
  materialized (deep-copied; long-string bytes into the table's heap) because the
  build side is a pipeline breaker and input chunks are borrowed. A matched row is
  gathered from one contiguous region.
- **Equi-join NULL semantics** (not 3VL): build rows with a NULL join key are
  *excluded*; probe rows with a NULL key are *skipped* — so the key compare needs
  no NULL logic.
- **Probe**: hash → walk the chain → quick-reject on the stored 64-bit hash →
  full key compare. **Fan-out** (a probe row may match many build rows) is buffered
  to `kVectorSize` and flushed via a gather: probe columns by a selection vector
  over the live probe chunk, build columns from the matched rows. The walk/gather is
  random-access (memory-bound, not a SIMD win — ADR 0008's ceiling).
- Multi-key (composite hashing). Cross-checked against DuckDB. Build goes parallel
  in P8 (foreshadowed: thread-local partitions or a shared table, read-only probe).

## Sort / Limit / Top-N (P6)

[ADR 0013](adr/0013-sort-limit-topn.md). All three are sinks; the engine is now
genuinely usable (a safe checkpoint).

- **`Sort`** (`ORDER BY`) — a pipeline breaker: materialize all input (repacked
  into the operator's own 2048-row chunks for O(1) `idx → (chunk, offset)`),
  `std::stable_sort` an **index** array by a multi-key comparator (sort indices,
  not wide rows), then gather rows in order. Stable; multi-column ASC/DESC; NULL
  ordering via `nulls_first` — an *absolute* position (default NULLS LAST, both
  directions, matching DuckDB), independent of ASC/DESC which flips only the value
  compare. Shared `CompareRows`.
- **`TopN`** (`ORDER BY … LIMIT k`) — a **bounded max-heap** of size k (root =
  worst kept row; a better row replaces it). O(N log k) time, O(k) memory vs the
  full sort's O(N log N)/O(N). Same comparator → output equals
  full-sort-then-LIMIT-k (tested invariant).
- **`Limit`** — forwards the first n rows. Early termination (stop the source) is
  the deferred push "stop" signal (ADR 0001), so a bare LIMIT still drains input.
- **Deferred**: normalized/binary sort keys (one `memcmp` over an order-preserving
  encoded key) — the known next optimization, documented in ADR 0013.

## SQL front-end & optimizer (P7)

The end-to-end `query(sql, catalog) → result` path ([ADR 0014](adr/0014-sql-frontend-and-optimizer.md)):

- **Parser** (`plan/parser`) — a hand-written tokenizer + recursive-descent parser
  over Strata's executable SQL subset (the sanctioned fallback over a library — see
  the ADR for the rationale). Produces an *unbound* `LogicalNode` tree. `BETWEEN`
  desugars to `>= AND <=`.
- **`LogicalPlan` IR + binder + catalog** — relational-algebra nodes (Get, Filter,
  Project, Aggregate, Join, Order, Limit) decoupled from physical operators. The
  binder resolves column names against a `Catalog` of registered tables, computes
  each node's output schema, and coerces literal constants to the operand type.
- **Optimizer** (`plan/optimizer`) — rule-based: **constant folding**, **predicate
  pushdown** (split conjuncts; push below joins to the referenced side — the
  "filter below the join" rewrite), and **projection pushdown** (prune a Get to
  the columns actually referenced — the columnar win). No cost-based join
  reordering (deferred).
- **Executor** (`plan/executor`) — lowers the optimized plan to the physical
  operators (Scan/Filter/Project/HashAggregate/Sort/TopN/Limit), driving the scan
  through Filter + a projection into a chain of sinks (`ORDER BY … LIMIT` fuses to
  `TopN`). Single-table queries run end-to-end and are cross-checked against
  DuckDB; **JOIN execution is deferred to P9** (the Join node + the inner-join
  pushdown rule exist and are tested at the IR level).

## Morsel-driven parallelism (P8)

[ADR 0015](adr/0015-morsel-driven-parallelism.md). The marquee piece, **TSan-clean**.

- **`ThreadPool`** (`parallel/thread_pool`) — work-stealing: per-worker
  mutex-guarded deques (run own LIFO, steal FIFO), parked on a condition variable,
  with `ParallelFor(n, body(task, worker_id))` as a fork-join barrier. Mutex/atomic/
  cv synchronization → TSan-clean by construction.
- **`ParallelAggregate`** (`parallel/parallel_aggregate`) — morsel-driven GROUP BY:
  the table's chunks are morsels; each worker folds its morsels into a
  **thread-local `HashAggregate`** (no shared mutable state); after the barrier a
  single-threaded **`MergeFrom`** combines the per-worker partials at the **raw
  state level** (a new per-aggregate `combine` op — combines `(sum,count)`, not
  finalized values) and `Finalize`s to the output.
- **Verified**: bit-identical to serial for integer aggregates at 1/2/4/8 threads;
  TSan-clean under a stress harness; **8.1× at 11 threads** (sub-linear — the M3's
  E-cores + memory-bound aggregation; see [BENCHMARKS](BENCHMARKS.md#parallel-aggregation-scaling--neon-p8)).
- **Honest boundary**: this is a standalone operator — the SQL executor still
  aggregates serially; routing the planner through it is the next step.

## TPC-H validation against DuckDB (P9)

[ADR 0016](adr/0016-tpch-validation-and-the-gap.md). Strata's executor runs
single-table plans, so the runnable TPC-H subset is **Q1** (aggregation/expression
throughput) and **Q6** (scan/filter) — neither needs a join.

- **Harness** (`bench/tpch_runner`): loads the 7 `lineitem` columns Q1/Q6 reference
  from CSV (decimals → `DOUBLE`; Strata has no `DECIMAL`), runs both via `query()`,
  validates each against the DuckDB oracle (columns cast to `DOUBLE` for an
  apples-to-apples comparison), and times them.
- **Result**: at SF1 (6M rows), Q1/Q6 match DuckDB to ~13 significant figures
  (low-digit deltas are non-associative double summation order). Strata is
  **~3–7× slower than single-threaded DuckDB, ~18–20× vs. its multi-threaded
  default** — measured and explained per query in [BENCHMARKS](BENCHMARKS.md).
- **Regression test** (`tests/test_tpch`): a 12-row in-code `lineitem` with
  DuckDB-baked expected values, so CI re-validates Q1/Q6 without needing DuckDB.
- **Deferred (honest)**: the other 20 queries need join execution (operator exists,
  ADR 0012), a `DECIMAL` type, and more SQL; the parallel layer isn't wired into
  `query()`.

## Module map (fills in by phase)

| Module | Phase | Status |
|--------|-------|--------|
| `common/` (Result, Error, macros) | P0 | done |
| `simd/cpu_features` (preflight) | P0 | done |
| `data/` — `TypeId`, `Validity`, `StringRef`/`StringHeap`, `SelectionVector`, `Vector`, `DataChunk` | P1 | done |
| `storage/` (`Schema`, `ColumnarTable`, delimited loader) + `exec/` (`Source`/`Sink`/`Pipeline`, `Scan`, `ResultCollector`) | P2 | done |
| `simd/` (Highway kernels + scalar ref) + `exec/` (`ExpressionExecutor`, `Filter`, `Project`) | P3 | done |
| `exec/aggregate` + `exec/hash_aggregate` (open-addressing GROUP BY) | P4 | done |
| `exec/hash_join` (chained build/probe equi-join) | P5 | done |
| `exec/sort` + `exec/top_n` + `exec/limit` | P6 | done |
| `plan/` — parser, `LogicalPlan` IR, binder + catalog, optimizer, executor, `query()` | P7 | done |
| `parallel/` — work-stealing `ThreadPool` + `ParallelAggregate` (morsel-driven) | P8 | built + TSan-clean (executor integration pending) |
| `bench/tpch_runner` + `tests/test_tpch` — TPC-H Q1/Q6 validated vs. DuckDB | P9 | done (single-table subset) |
