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

## 5. Parallel scaling is sub-linear on heterogeneous cores *(measured from P8)*

The M3 Pro has 5 performance + 6 efficiency cores. E-cores are slower, so
morsel-driven scaling will not be perfectly linear; the scaling curve in
`BENCHMARKS.md` will show and explain this rather than quoting an idealized
"N×" number.

## 6. Scope of SQL support

Strata implements a *subset* of SQL sufficient to run a representative set of
TPC-H queries. Unsupported features (e.g. correlated subqueries, window
functions, full DDL/DML) are listed explicitly as they are decided, with the
queries they exclude. *(enumerated in P7/P9)*
