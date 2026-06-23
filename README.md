# Strata

**A mini-DuckDB: a vectorized, columnar, push-based analytical query engine in C++23 — validated against DuckDB itself for both correctness and performance.**

Strata executes analytical SQL over in-memory columnar data using *vectorized
execution*: data flows through operators in cache-resident batches ("vectors")
of `VECTOR_SIZE = 2048` values rather than one tuple at a time, amortizing
interpretation overhead and exposing data-level parallelism to the CPU. This is
the architecture pioneered by MonetDB/X100 and used today by DuckDB and
Databricks Photon.

DuckDB plays three roles in this project at once:
- **Data generator** — TPC-H data via its `tpch` extension (`CALL dbgen(...)`).
- **Correctness oracle** — every supported query is diffed against DuckDB's result.
- **Performance target** — we benchmark against it and explain every gap honestly.

> Strata is a learning-grade reimplementation of the core ideas behind DuckDB.
> It is **slower than DuckDB** on most queries — that is expected. The goal is a
> correct, sanitizer-clean engine whose performance gaps are *measured and
> explained*, not hidden. See [`docs/LIMITATIONS.md`](docs/LIMITATIONS.md).

## The three things this project is really about

1. **Portable SIMD with honestly measured deltas** — real kernels behind a
   runtime-dispatch abstraction (Google Highway), with benchmarks that show
   where SIMD wins big (selective filters, numeric arithmetic) *and* where it
   barely helps (strings, hash probes, branchy code).
2. **Morsel-driven parallelism** (Leis et al., SIGMOD 2014) — a work-stealing
   scheduler that scales across physical cores, TSan-clean.
3. **TPC-H benchmarked against DuckDB**, with a written gap analysis.

## Status

Early. Built in gated phases (P0–P9); see the roadmap below. **P0–P8 are
complete**: the toolchain/CI skeleton (P0), the columnar data plane (P1),
storage + scan + the push-based pipeline (P2), the vectorized expression engine
with NULL-aware Filter/Project and Highway SIMD kernels (P3), hash aggregation
(P4), the hash join (P5), Sort / Limit / Top-N (P6), the SQL front-end with an
end-to-end `query(sql)` path (P7), and **morsel-driven parallelism** — a
work-stealing pool with thread-local aggregation + merge (P8). **138 tests**
green under ASan/UBSan and **TSan**; aggregates, joins, sort order, and full SQL
results cross-checked against DuckDB.

Measured numbers in [`docs/BENCHMARKS.md`](docs/BENCHMARKS.md): scalar-vs-SIMD on
NEON (~3.7× on int32, ~1.7× on doubles — lane-count bounded), and parallel
aggregation scaling (**~8× at 11 threads** — sub-linear, honestly attributed to
the M3's efficiency cores + memory-bound aggregation). The parallel layer is a
TSan-clean standalone operator; wiring it behind the SQL planner, and the full
TPC-H-vs-DuckDB comparison + gap analysis, come in P9.

## Build

Requires a C++23 toolchain. On macOS use **Homebrew LLVM** (Apple clang lags on
C++23 library features — see [ADR 0002](docs/adr/0002-cxx23-homebrew-llvm-toolchain.md)).

```bash
brew install llvm cmake ninja highway googletest google-benchmark duckdb

# Debug build with ASan + UBSan, then run the tests:
cmake --preset asan-ubsan
cmake --build --preset asan-ubsan
ctest --preset asan-ubsan

# Optimized build, then the SIMD preflight:
cmake --preset release
cmake --build --preset release
./build/release/src/strata --version
```

Presets: `release`, `asan-ubsan`, `tsan` (macOS, Homebrew LLVM); `linux-release`,
`linux-asan-ubsan`, `linux-tsan` (Linux CI, compiler via `CC`/`CXX`).

## Architecture (target)

```
SQL ──► Parser ──► Logical plan ──► Optimizer ──► Physical plan ──► Pipelines
                                   (pushdown,                          │
                                    const-fold)                        ▼
                         Scan ─► Filter ─► Project ─► [Sink: HashAggregate /
                         (push DataChunks of 2048 values)   HashJoin / Sort / Collect]
```

- **Type system:** `INT32, INT64, DOUBLE, BOOL, VARCHAR, DATE` (+ `DECIMAL` if a target query needs it).
- **Vector:** `{type, data, validity bitmask, optional selection vector}`.
- **DataChunk:** equal-length set of Vectors (≤ 2048) — the unit between operators.
- **Execution:** push-based pipelines into sinks (see [ADR 0001](docs/adr/0001-push-based-execution.md)).

## Roadmap (one gated phase at a time)

| Phase | Content |
|-------|---------|
| **P0** ✅ | Toolchain, skeleton, CI (mac arm64 + ubuntu x86_64), `Result` type, SIMD preflight |
| **P1** ✅ | Columnar core: vectors, validity, DataChunk, selection vectors, string heap |
| **P2** ✅ | Storage + scan + push-based pipeline scaffolding (`SELECT * FROM t` end-to-end) |
| **P3** ✅ | Expression engine, Filter/Project, **Highway SIMD kernels + measured scalar-vs-SIMD deltas** |
| **P4** ✅ | Hash aggregation: open addressing + salt + row layout; COUNT/SUM/MIN/MAX/AVG, NULL + overflow handling |
| **P5** ✅ | Hash join: chained build table + row layout, vectorized match gather, multi-key, NULL semantics |
| **P6** ✅ | Sort / Limit / Top-N: stable comparator sort (multi-col, NULL order), bounded-heap Top-N |
| **P7** ✅ | SQL front-end: parser, logical plan IR, rule-based optimizer (predicate + projection pushdown), end-to-end `query()` |
| **P8** ✅ | **Morsel-driven parallelism**: work-stealing pool + thread-local aggregation + merge, TSan-clean, ~8× at 11 threads (executor integration pending) |
| P9 | **TPC-H correctness + performance vs. DuckDB**, gap analysis |

## Documentation

- [`docs/adr/`](docs/adr/) — Architecture Decision Records (the design rationale / study notes).
- [`docs/DESIGN.md`](docs/DESIGN.md) — end-to-end architecture.
- [`docs/BENCHMARKS.md`](docs/BENCHMARKS.md) — numbers, machine specs, reproduction, gap analysis.
- [`docs/LIMITATIONS.md`](docs/LIMITATIONS.md) — honest boundaries (vectorization vs compilation, the SIMD ceiling, columnar-is-for-OLAP, Apple Silicon NEON reality).

## License

MIT — see [`LICENSE`](LICENSE).
