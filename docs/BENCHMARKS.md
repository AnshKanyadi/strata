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

### Microkernels: scalar vs. SIMD — *(P3)*
_Pending._

### TPC-H SF1 vs. DuckDB — *(P9)*
_Pending._

## Gap analysis

_Pending (P9)._ Will explain each delta vs. DuckDB with specific causes
(adaptive compression, tuned german-string layout, optimized hash tables,
expression rewriting, years of engineering), per query.
