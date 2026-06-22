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

### TPC-H SF1 vs. DuckDB — *(P9)*
_Pending._

## Gap analysis

_Pending (P9)._ Will explain each delta vs. DuckDB with specific causes
(adaptive compression, tuned german-string layout, optimized hash tables,
expression rewriting, years of engineering), per query.
