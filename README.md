# Strata

Strata is an analytical SQL engine I wrote from scratch in C++23 to really understand how a modern column store like DuckDB works on the inside. It runs queries over in-memory columnar data using vectorized execution, so data moves through the operators in batches of 2048 values at a time instead of one row at a time. That batching is the whole point. It keeps the inner loops tight, gives the compiler and the CPU room to do actual SIMD work, and gets rid of the per-row interpreter overhead that makes a naive engine slow.

I built and checked the whole thing against DuckDB. Every query Strata can run is diffed against DuckDB's answer, and every performance number in this repo comes from a real run on my machine, not a guess. Strata is slower than DuckDB, and I say by how much and why. Beating it was never the goal. The goal was a correct, sanitizer-clean engine built up one piece at a time, with the gaps measured instead of hidden.

## How a query flows through it

Strata takes a SQL string to a result the way a real database does, one stage at a time:

```
SQL text
  -> tokenizer + recursive descent parser
  -> logical plan (relational algebra IR)
  -> binder (resolve names against the catalog, assign types)
  -> optimizer (constant folding, predicate pushdown, projection pushdown)
  -> physical operators
  -> push-based pipeline into a sink
  -> materialized result
```

The physical layer is push-based. A scan pushes chunks of at most 2048 rows into a filter, the filter into a projection, and so on, until the data lands in a sink that breaks the pipeline: a hash aggregate, a hash join, a sort, or a collector. Sinks are where the work accumulates and where a pipeline stops. I went with push instead of pull partly because it composes cleanly with parallelism later on, and that turned out to be the right call when I got to the morsel scheduler.

## The pieces

**Data plane.** Columns are stored as typed `Vector`s backed by 64-byte-aligned buffers. NULLs live in a separate validity bitmask so the common all-valid case costs nothing. Strings use an inline layout in the spirit of the Umbra and DuckDB "german strings": up to 12 bytes sit right inside a 16-byte reference, and longer strings keep a 4-byte prefix plus a pointer into an arena, so a lot of comparisons never touch the heap. A `SelectionVector` lets a filter narrow a chunk without copying the underlying columns.

**Expressions.** A vectorized evaluator handles arithmetic, comparisons, and boolean logic over whole chunks. NULL handling follows real three-valued logic all the way through, which is one of those things that looks simple and absolutely is not once `AND`, `OR`, and `NOT` start interacting with unknowns.

**SIMD.** The comparison and arithmetic kernels use Google Highway so the same source compiles to NEON on my Mac and to AVX2 (or wider) on x86, with the target chosen at runtime. I benchmarked scalar against SIMD on identical inputs and wrote down where it helps and where it does not.

**Aggregation.** GROUP BY runs through an open-addressing hash table with a salt byte per slot and a row-oriented payload layout, so a probe usually rejects a non-match after one byte and one cache line. It supports COUNT, SUM, MIN, MAX, and AVG with correct NULL semantics and a defined integer overflow policy.

**Join.** Inner equi-join with a chained build side and a vectorized probe that gathers matches in bulk. Multi-key and NULL semantics match SQL.

**Sort, Top-N, Limit.** A stable comparator sort over multiple keys with configurable NULL ordering, plus a bounded max-heap for Top-N that never materializes more than it needs.

**Parallelism.** A work-stealing thread pool with per-worker deques drives morsel-driven aggregation. Each worker folds its slice of the table into its own thread-local hash table with no shared mutable state, and a single merge step combines the partial tables at the end. It is clean under ThreadSanitizer.

## Why DuckDB is all over this repo

DuckDB does three jobs here at once:

- It generates the data. TPC-H comes straight from its `tpch` extension.
- It is the correctness oracle. If Strata and DuckDB disagree on a result, Strata is wrong.
- It is the performance target. I measure against it and explain the difference.

## The numbers

Everything below is from an actual run on an Apple M3 Pro (5 performance cores, 6 efficiency cores, ARM NEON), Homebrew LLVM clang, release build. Full detail, machine specs, and reproduction steps are in [`docs/BENCHMARKS.md`](docs/BENCHMARKS.md).

**SIMD kernels (NEON, scalar vs vectorized).** Around 3.7x on int32 arithmetic and comparisons, around 1.7x on doubles. The 32-bit-vs-64-bit split is not an accident: a 128-bit NEON register holds four int32 but only two doubles, so you get about half the lane parallelism and about half the speedup. I state the honest caveat too, which is that the scalar baseline has autovectorization turned off, so these ratios are an upper bound on the win over what the compiler would give you for free.

**Parallel aggregation.** A GROUP BY over 4 million rows scales from about 76 million rows per second on one thread to about 610 on eleven, roughly 8x. It is sub-linear, and I do not pretend otherwise: the efficiency cores are slower than the performance cores, and aggregation is memory bound, so the curve flattens well before eleven. On real TPC-H `lineitem` (6 million rows), a Q1-shaped grouped aggregation goes from 211 ms to 25 ms across eleven threads.

**TPC-H at scale factor 1 (6,001,215-row `lineitem`), validated against DuckDB.** Strata's executor runs single-table plans, so the queries it can run end to end are Q1 (the classic aggregation and expression query) and Q6 (the scan-and-filter query). Both match DuckDB to about thirteen significant figures. The remaining difference is floating point summation order, not an error, since IEEE addition is not associative and the two engines add the products in different orders.

| Query | Strata (serial) | DuckDB, 1 thread | DuckDB, default (11 threads) |
|-------|----------------:|-----------------:|-----------------------------:|
| Q6    | 141 ms          | ~21 ms (6.7x)    | ~7 ms (~20x)                 |
| Q1    | 658 ms          | ~232 ms (2.8x)   | ~37 ms (~18x)                |

So Strata is roughly 3x to 7x slower than single-threaded DuckDB and 18x to 20x slower than DuckDB running on all cores. The gap on Q6 is bigger because it is filter-heavy: DuckDB skips whole row groups with min/max metadata and pushes predicates into the scan, while Strata evaluates every predicate over all six million rows. Q1 is closer because both engines have to do the full grouped aggregation. The rest of the gap is mostly that DuckDB runs multi-threaded by default and Strata's query path is still serial. My parallel layer already does about 8x on the aggregation on its own, it just is not wired behind the planner yet.

## How I built it

I built Strata in ten stages and treated each one as a gate: it does not move on until it compiles with zero warnings under `-Werror`, all tests pass, and the whole thing is clean under AddressSanitizer, UndefinedBehaviorSanitizer, and (once threads showed up) ThreadSanitizer. That discipline paid off more than once. ASan caught a dangling `string_view` in the sort comparator that I would never have found by reading the code, and TSan gave me real confidence that the work-stealing pool was actually race-free rather than just lucky.

For every non-obvious decision I wrote a short Architecture Decision Record explaining the choice, the alternatives, and the tradeoff. There are sixteen of them in [`docs/adr/`](docs/adr/), and they double as my own notes on why things are the way they are.

| Stage | What landed |
|-------|-------------|
| 0 | Toolchain, build system, CI for mac arm64 and ubuntu x86_64, the `Result` type, SIMD preflight |
| 1 | Columnar core: vectors, validity masks, data chunks, selection vectors, string heap |
| 2 | Storage, scan, and the push-based pipeline (`SELECT * FROM t` end to end) |
| 3 | Expression engine, Filter and Project, Highway SIMD kernels with measured deltas |
| 4 | Hash aggregation with open addressing, salt bytes, and a row layout |
| 5 | Hash join: chained build table, vectorized probe, multi-key, NULL semantics |
| 6 | Sort, Limit, and Top-N |
| 7 | SQL front end: parser, logical plan, rule-based optimizer, end-to-end `query()` |
| 8 | Morsel-driven parallelism: work-stealing pool, thread-local aggregation, merge |
| 9 | TPC-H Q1 and Q6 validated against DuckDB, with a written gap analysis |

## Building it

You need a C++23 toolchain. On macOS use Homebrew LLVM, since Apple's clang lags on the C++23 library features Strata leans on (`std::expected` in particular). The reasoning is in [ADR 0002](docs/adr/0002-cxx23-homebrew-llvm-toolchain.md).

```bash
brew install llvm cmake ninja highway googletest google-benchmark duckdb

# debug build with ASan and UBSan, then the tests
cmake --preset asan-ubsan
cmake --build --preset asan-ubsan
ctest --preset asan-ubsan

# optimized build, then the SIMD preflight
cmake --preset release
cmake --build --preset release
./build/release/src/strata --version
```

There are presets for `release`, `asan-ubsan`, and `tsan` on macOS, and `linux-release`, `linux-asan-ubsan`, and `linux-tsan` for CI. All 141 tests pass under the debug, release, and TSan builds.

To run the TPC-H harness, generate the data with DuckDB and point the runner at it:

```bash
duckdb -c "INSTALL tpch; LOAD tpch; CALL dbgen(sf=1);
  COPY (SELECT l_quantity, l_extendedprice, l_discount, l_tax,
               l_returnflag, l_linestatus, l_shipdate FROM lineitem)
  TO '/tmp/lineitem_sf1.csv' (HEADER false);"
./build/release/bench/strata_tpch /tmp/lineitem_sf1.csv
```

## Layout

```
include/strata/    public headers, grouped by layer (data, simd, exec, plan, parallel)
src/               implementations + the strata CLI
tests/             GoogleTest suites, one per component
bench/             Google Benchmark microbenchmarks + the TPC-H and parallel harnesses
docs/adr/          16 architecture decision records
docs/              DESIGN, BENCHMARKS, LIMITATIONS
```

## What is not done yet

I would rather be clear about the edges than let you find them by surprise. The full list is in [`docs/LIMITATIONS.md`](docs/LIMITATIONS.md), but the big ones:

- The parser and executor do not wire up joins yet. The hash-join operator itself is built and tested in isolation, but SQL that joins two tables does not run end to end. That is why the TPC-H work covers the two single-table queries and not the twenty that need a join.
- The parallel layer is validated on its own but is not plugged into the SQL query path, so `query()` still aggregates on one thread.
- There is no DECIMAL type, so money columns load as doubles. The TPC-H validation compares double against double for that reason.
- SQL coverage is a deliberate subset: no subqueries, no HAVING, no window functions, no LIKE.

## Reading the design notes

- [`docs/DESIGN.md`](docs/DESIGN.md) walks the architecture end to end.
- [`docs/BENCHMARKS.md`](docs/BENCHMARKS.md) has the numbers, the machine, how to reproduce them, and the gap analysis.
- [`docs/LIMITATIONS.md`](docs/LIMITATIONS.md) is the honest boundaries: vectorization versus compilation, the SIMD ceiling, why columnar is an OLAP choice, and the Apple Silicon core-asymmetry reality.
- [`docs/adr/`](docs/adr/) is the decision-by-decision rationale.

## License

MIT. See [`LICENSE`](LICENSE).
