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

Early. Built in gated phases (P0–P9); see the roadmap below. **P0 (toolchain,
skeleton, CI) and P1 (the columnar data plane — vectors, validity, string heap,
selection vectors, DataChunk) are complete**, with 44 tests green under
ASan/UBSan and TSan. No performance numbers are published yet — they arrive in P3 (kernels) and P9 (TPC-H), measured
on the harness and recorded with machine specs in
[`docs/BENCHMARKS.md`](docs/BENCHMARKS.md).

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
| P2 | Storage + scan + push-based pipeline scaffolding |
| P3 | Expression engine, Filter/Project, **first SIMD kernels + measured deltas** |
| P4 | Hash aggregation (open addressing + salt + row layout) |
| P5 | Hash join |
| P6 | Sort / Limit / Top-N |
| P7 | Plan IR, rule-based optimizer, SQL front-end |
| P8 | **Morsel-driven parallelism** (work-stealing, TSan-clean) |
| P9 | **TPC-H correctness + performance vs. DuckDB**, gap analysis |

## Documentation

- [`docs/adr/`](docs/adr/) — Architecture Decision Records (the design rationale / study notes).
- [`docs/DESIGN.md`](docs/DESIGN.md) — end-to-end architecture.
- [`docs/BENCHMARKS.md`](docs/BENCHMARKS.md) — numbers, machine specs, reproduction, gap analysis.
- [`docs/LIMITATIONS.md`](docs/LIMITATIONS.md) — honest boundaries (vectorization vs compilation, the SIMD ceiling, columnar-is-for-OLAP, Apple Silicon NEON reality).

## License

MIT — see [`LICENSE`](LICENSE).
