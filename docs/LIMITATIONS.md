# Limitations & honest boundaries

This document states — prominently and up front — what Strata is *not*, and the
tradeoffs baked into its design. Overclaiming gets a project dismantled in an
interview; stating the boundary precisely is the credibility signal. Every point
here is a deliberate, defensible engineering choice, not an accident.

> This file is seeded in P0 and expanded as each phase ships. Items marked
> *(pending)* will be backed by measured numbers once the relevant phase lands.

## 1. Vectorization, not compilation

Strata is a **vectorized** (interpreted-per-batch) engine, like MonetDB/X100,
DuckDB, and Photon — *not* a **compiling/JIT** engine like HyPer or Spark SQL's
whole-stage codegen. Kersten et al. (VLDB 2018, *"Everything You Always Wanted to
Know About Compiled and Vectorized Queries But Were Afraid to Ask"*) found the
two approaches are broadly competitive, with each winning on different query
shapes, so the choice is an engineering one. We chose vectorization for:

- **Debuggability and profiling** — ordinary C++ you can step through and
  attribute time to, with no generated code to inspect.
- **No JIT warm-up / compile latency** — predictable first-query latency.

This is the same reasoning Databricks gives for choosing vectorization for
Photon *inside* a JVM-based Spark. The tradeoff we accept: a compiler can fuse an
entire pipeline into one tight loop and specialize away interpretation overhead
that we pay per batch.

## 2. Columnar is for OLAP, not OLTP

A columnar layout wins for analytical scans that touch a few columns across many
rows. It is the *wrong* choice for point lookups and row-at-a-time writes, where
a row store wins. Strata is an in-memory analytical engine and makes no claim to
be a general-purpose or transactional database. No durability, no concurrency
control, no updates/deletes.

## 3. SIMD has a real ceiling *(measured in P3)*

SIMD accelerates branch-free numeric and comparison loops, and **little else.**
The P3 microbenchmarks measure this on NEON (full table + caveats in
[`BENCHMARKS.md`](BENCHMARKS.md#microkernels-scalar-vs-simd--neon-p3)):

- **Lane count is the ceiling.** int32 `+`/`*`/`>` win ~3.7× (NEON packs 4×int32);
  the *same* ops on `double` win only ~1.7× (2×double per 128-bit vector). Half
  the lanes, half the speedup — the win is bounded by the vector width, not by
  cleverness.
- **It does little for** strings (variable length, out-of-line, compare is a
  byte loop not a lane op — ADR 0004), hash-table probes (random gather /
  pointer chasing), and the three-valued logical ops AND/OR/NOT (data-dependent
  branches). These run in **scalar** by design; there is no honest SIMD speedup
  to show, so we don't fabricate one.
- **The "scalar" baseline disables auto-vectorization.** At `-O3` the compiler
  would auto-vectorize the simple scalar loops itself, so the measured ratios are
  an *upper bound* on the gain over compiler-auto-vectorized scalar. We say so
  next to the numbers rather than implying SIMD is uniquely responsible.

## 4. Apple Silicon NEON reality

The development machine is an Apple M3 Pro: ARM **NEON is 128-bit** (4×`int32`
per lane group), with no AVX. So local speedups are more modest than the
AVX2/AVX-512 figures in the literature. Highway compiles the *same* kernel
sources to AVX2 on the x86_64 CI runner, so [`BENCHMARKS.md`](BENCHMARKS.md)
reports **both** NEON (macOS) and AVX2 (Linux) numbers rather than extrapolating.

## 5. Parallelism: scope and honest scaling *(P8)*

The morsel-driven parallel layer (work-stealing thread pool + thread-local
aggregation + merge, ADR 0015) is **built, TSan-clean, and bit-identical to
serial** for integer aggregates. Honest boundaries:

- **Sub-linear on the M3's asymmetric cores.** 5 performance + 6 efficiency cores;
  E-cores are slower and aggregation is memory-bound, so measured scaling is
  ~3.7× at 4 threads, 5.4× at 6, **8.1× at 11** — not ~11× ([`BENCHMARKS.md`](BENCHMARKS.md#parallel-aggregation-scaling--neon-p8)).
- **Not yet wired into the SQL executor.** `query()` still aggregates serially;
  `ParallelAggregate` is validated as a standalone operator. Routing the planner
  through it is the next step.
- **Mutex-guarded work-stealing deques**, not a lock-free Chase-Lev deque
  (correctness/TSan-cleanliness over the last bit of throughput) — deferred.
- **Floating-point SUM/AVG are not bit-identical across thread counts**: parallel
  summation order differs and IEEE addition is non-associative, so results may
  differ in the last ULP. Integer aggregates *are* bit-identical. No Kahan/
  compensated summation.
- **Parallel filter/project/join** are not wired (filter/project are
  embarrassingly parallel via the same `ParallelFor`; parallel join build lands
  with join execution in P9).
- **No NUMA awareness**: the M3 is a single unified-memory domain, so Leis's
  NUMA-local queue placement is moot on-device and not validated anywhere.

## 6. Scope of SQL support *(P7)*

The parser is hand-written over a deliberate **subset** (ADR 0014; the build
spec's sanctioned fallback over a parser library, chosen for exact grammar
control and a clean two-compiler CI). Validated end-to-end against DuckDB.

**Supported:** `SELECT` of column refs / `*` / arithmetic / the five aggregates
(`COUNT`/`SUM`/`MIN`/`MAX`/`AVG`) with `AS`; `FROM` one table; `WHERE`,
`GROUP BY`, `ORDER BY` (per-key `ASC`/`DESC`, `NULLS FIRST`/`LAST`), `LIMIT`;
`AND`/`OR`/`NOT`, comparisons, `+ - *`, `BETWEEN` (desugared); int/double/string/
`DATE 'YYYY-MM-DD'` literals.

**Not supported (stated plainly, not accidental):**
- **JOINs do not yet *execute*** — the hash-join *operator* exists (ADR 0012,
  tested in isolation) and the `Join` logical node + predicate-pushdown-through-join
  rule are built/tested at the IR level, but the **parser and executor do not wire
  joins** end-to-end. This is the single largest remaining gap, and it is why the
  TPC-H validation (P9) covers only the **single-table** queries Q1 and Q6, not the
  20 join-requiring queries. Stated plainly, not glossed.
- **`ORDER BY` must reference a selected column** (the parser places `ORDER BY`
  above the projection; ordering by a non-projected column is not supported).
- No subqueries, `HAVING`, `DISTINCT`, set operations, window functions, `CASE`,
  `LIKE`, division, or DDL/DML.
- The optimizer is **rule-based, not cost-based**: no statistics and no join
  reordering — a bad join order is not corrected (deferred, ADR 0014).
- The binder does only literal-constant coercion; there are no implicit
  column-to-column casts.
