# Benchmarks

> **No performance numbers are published yet.** Strata is at P0 (skeleton). This
> file defines the methodology now so that when numbers arrive (P3 microkernels,
> P9 TPC-H) they are honest, reproducible, and comparable. **Every figure here
> will come from an actual harness run, with the machine and config recorded —
> nothing is estimated, rounded up, or cherry-picked.**

## Machines

| Role | CPU | SIMD | Cores | OS / Compiler |
|------|-----|------|-------|---------------|
| Local (dev) | Apple M3 Pro | NEON (128-bit) | 5 P + 6 E (11 total) | macOS, Homebrew LLVM clang |
| CI x86_64 | GitHub `ubuntu-latest` runner | AVX2 (≥) | (runner-dependent; recorded per run) | Ubuntu, GCC 14 |

The exact CPU model, core counts, and the SIMD target Highway dispatched to are
captured by `strata --version` and pasted alongside each result set.

## Methodology (to be applied from P3 on)

- **Tool:** Google Benchmark for microbenchmarks; a dedicated harness for TPC-H.
- **Build:** `release` preset (`-O3 -march=native -DNDEBUG`), sanitizers OFF.
- **Warmup + repetition:** report median of repeated runs with min-time floors;
  record variance, not just the best run.
- **What is measured:** wall-clock per operation/query; for SIMD, scalar vs.
  dispatched-SIMD on identical inputs (including NULLs).
- **Baseline:** DuckDB (version recorded), same query, single- and multi-threaded.

## Reproduction

```bash
cmake --preset release && cmake --build --preset release
./build/release/src/strata --version          # records machine + SIMD target
./build/release/bench/strata_bench             # microbenchmarks (P3+)
# TPC-H harness command: (added in P9)
```

## Results

### Microkernels: scalar vs. SIMD — NEON (P3)

Machine: **Apple M3 Pro**, ARM NEON 128-bit (`NEON_BF16` dispatched), Homebrew
LLVM clang 22.1.7, `release` preset (`-O3 -march=native -DNDEBUG`). Batch =
`kVectorSize` = 2048 values. Throughput = values processed per second (higher is
better); reproduce with `./build/release/bench/strata_bench_kernels`.

> **Honesty caveat on the baseline.** The "scalar" column has auto-vectorization
> **disabled** (`#pragma clang loop vectorize(disable)`), so these ratios isolate
> the SIMD kernel's contribution against *genuinely scalar* code. At `-O3` the
> compiler would auto-vectorize these simple loops on its own, so the speedup
> over *naive* scalar shown here is an **upper bound** on the gain over what the
> optimizer already gives you for free. The point of the table is the *shape* —
> where SIMD helps a lot vs. a little — not the absolute multiplier.

| Kernel | Scalar (no autovec) | Highway SIMD | Speedup |
|--------|--------------------:|-------------:|--------:|
| `+`  int32  | 2.48 G/s | 8.90 G/s | **3.6×** |
| `*`  int32  | 2.45 G/s | 9.15 G/s | **3.7×** |
| `+`  double | 2.52 G/s | 4.46 G/s | **1.8×** |
| `>`  int32 → uint8 | 2.32 G/s | 8.74 G/s | **3.8×** |
| `>`  double → uint8 | 2.57 G/s | 4.07 G/s | **1.6×** |

**Reading the ceiling (the honest part):**
- **32-bit kernels win ~3.7×; doubles win only ~1.7×.** NEON is 128-bit, so it
  packs **4× int32** but only **2× double** per vector — about half the lane
  parallelism, hence about half the speedup. This is the single clearest
  illustration of the SIMD ceiling in the data: the win is bounded by lane count.
- **Comparison ≈ arithmetic, per type.** The int32 comparison (3.8×) matches
  int32 arithmetic (3.6–3.7×), and the double comparison (1.6×) tracks double
  arithmetic (1.8×). The `DemoteTo` narrowing to a uint8 result column (ADR 0008)
  costs little here; lane count dominates, not the narrowing.
- **What is *not* here, and why** (see [`LIMITATIONS.md`](LIMITATIONS.md#3-simd-has-a-real-ceiling-measured-from-p3)):
  string comparison, hash probes, and the three-valued logical ops (AND/OR/NOT)
  run in **scalar** by design — variable length, random gather, and
  data-dependent branches defeat lane parallelism, so SIMD buys little. We do not
  benchmark a SIMD version of those because there is no honest speedup to show.

AVX2 numbers from the x86_64 CI runner will be added once CI runs (the same
Highway source, wider vectors — expect larger int32 gains and a similar
32-vs-64-bit gap).

### Parallel aggregation scaling — NEON (P8)

Morsel-driven `ParallelAggregate` (work-stealing pool + thread-local hash tables +
merge, ADR 0015), GROUP BY over **4,000,000 rows / 1,000 groups**, `SUM`+`COUNT(*)`,
on the **Apple M3 Pro (5 performance + 6 efficiency cores, 11 total)**, `release`
preset. Median of 5 iterations; reproduce with
`./build/release/bench/strata_bench_parallel`.

| Threads | Mrows/s | Speedup vs. 1 |
|--------:|--------:|--------------:|
| 1  | 76  | 1.00× |
| 2  | 153 | 2.02× |
| 4  | 279 | 3.70× |
| 6  | 411 | 5.44× |
| 8  | 479 | 6.34× |
| 11 | 610 | 8.08× |

**Reading it honestly:** scaling is near-linear through ~4 threads and still strong
at 6 (5.4×) — roughly the **5 performance cores** plus one efficiency core. Beyond
that it goes **sub-linear** (6.3× at 8, **8.1× at 11**, not ~11×): the M3's
**efficiency cores are slower than its performance cores**, and aggregation is
memory-/hash-probe-bound (ADR 0008 ceiling), so adding E-cores yields diminishing
returns. This is exactly the asymmetric-core behaviour the ADR predicts — reported,
not rounded up to "near-linear to 11×".

> Scope note: this measures the standalone `ParallelAggregate` operator. The SQL
> executor (`query()`) still aggregates serially — routing the planner through the
> parallel layer is the next step (ADR 0015).

### TPC-H SF1 vs. DuckDB (P9)

**Setup.** Data: DuckDB v1.5.3 `tpch` extension, `CALL dbgen(sf=1)` →
`lineitem` = **6,001,215 rows**. Strata loads the **7 projected columns** Q1/Q6
reference (`l_quantity, l_extendedprice, l_discount, l_tax, l_returnflag,
l_linestatus, l_shipdate`) from CSV into an in-memory `ColumnarTable` (~1.6 s).
The runnable TPC-H subset is the **single-table** queries — **Q1** (the canonical
aggregation/expression query) and **Q6** (scan/filter) — because Strata's executor
runs single-table plans (join execution is deferred; ADR 0014/0016). Strata has no
DECIMAL type, so decimals load as **DOUBLE**; the DuckDB oracle is run with the
same columns **cast to DOUBLE** for an apples-to-apples comparison. Strata timings:
median of 7, `release` preset, M3 Pro / NEON. Reproduce:
`./build/release/bench/strata_tpch /tmp/lineitem_sf1.csv`.

**Correctness — validated against DuckDB (ALL PASS).** Q6 = `123141078.22829895`
(Strata) vs `123141078.2282996` (DuckDB double) — match to ~13 significant
figures; the low-digit difference is double **summation order**, not error (IEEE
add is non-associative). DuckDB's *native-decimal* Q6 is `123141078.2283` — the
honest decimal-vs-double representational delta. Q1's four `(returnflag,linestatus)`
group rows all match within ~`3e-14` relative.

**Performance — the honest gap.**

| Query | Strata (serial) | DuckDB 1-thread | DuckDB default (11-thread) |
|-------|----------------:|----------------:|---------------------------:|
| Q6 | 141 ms | ~21 ms (**6.7×**) | ~7 ms (**~20×**) |
| Q1 | 658 ms | ~232 ms (**2.8×**) | ~37 ms (**~18×**) |

(× = how much slower Strata is.) Strata is **~3–7× slower than single-threaded
DuckDB and ~18–20× slower than DuckDB's multi-threaded default** — an honest,
expected result for a from-scratch engine. **DuckDB runs multi-threaded by
default; Strata's query path is serial** — that is the single biggest factor.

**The parallel layer on real data.** The P8 morsel layer aggregates real
`lineitem` (a Q1-shaped `GROUP BY l_returnflag, l_linestatus`, `sum`+`count`)
**211 ms → 25 ms = 8.45×** at 11 threads. This is the **aggregation phase only**
(no `WHERE`, no per-morsel expression args), not the full Q1, and it is not yet
wired into `query()` — but it shows the gap to DuckDB's parallel default is
substantially parallelism, not algorithm.

## Gap analysis

- **Q6 (filter-heavy) shows the *larger* gap (6.7×).** Q6 is four predicates over
  6M rows feeding one sum. DuckDB pushes predicates into the scan with min-max /
  zonemap **skipping** (whole row groups are never touched) and runs a tight SIMD
  filter; Strata evaluates **every predicate over every row** with no skipping and
  no scan-level pushdown into storage. The gap here is mostly *work Strata does
  that DuckDB avoids*.
- **Q1 (aggregation-heavy) shows the *smaller* gap (2.8×).** Both engines must do
  the full grouped aggregation; Strata's vectorized hash-aggregate + expression
  evaluation is relatively competitive on the part that can't be skipped.
- **Multi-threading dominates the rest.** DuckDB's ~18–20× edge at its default
  thread count is mostly parallelism: Strata's `query()` is serial, and the
  standalone parallel layer already shows ~8× on the aggregation. Wiring it (plus
  per-morsel filter/projection) behind the planner is the documented next step.
- **What we are *not* claiming:** Strata is not competitive with DuckDB on speed,
  and it isn't meant to be. The deliverable is **validated correctness + a
  measured, explained gap**. Remaining DuckDB advantages not isolated here include
  adaptive/dictionary compression, more tuned hash tables, and expression
  rewriting — years of focused engineering.

AVX2 numbers from the x86_64 CI runner are pending; all numbers above are
M3 Pro / NEON.
