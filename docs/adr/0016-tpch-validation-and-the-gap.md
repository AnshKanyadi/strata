# 0016. TPC-H validation against DuckDB: the runnable subset, decimal-as-double, and the honest gap

## Status

**Accepted — as-built for P9, with a deliberately narrow, honestly-named scope.** What ran, end-to-end, through Strata's SQL `query()` path and was validated against DuckDB:

- **Q1** — the canonical TPC-H aggregation/expression-throughput query: `GROUP BY l_returnflag, l_linestatus` over the rows passing `WHERE l_shipdate <= date '1998-09-02'` (a high-selectivity filter that retains ~97% of rows), with multiple `SUM`/`AVG` over per-row arithmetic and `ORDER BY`. 4 group rows, all validated within a tight relative tolerance vs the DuckDB **double-domain** oracle. **658 ms** (median of 7) on SF1 `lineitem` (6,001,215 rows).
- **Q6** — the scan/filter query: a single-table conjunctive range filter with a global `SUM`. Validated against DuckDB double-domain to ~13 significant figures. **141 ms** (median of 7) on the same data.

Both are **single-table queries** — neither needs a JOIN — which is *exactly why they are the runnable subset* given that Strata's executor runs single-table plans only (ADR 0014). The claim is precise: **"we ran the two single-table TPC-H queries, correctly,"** not "we ran TPC-H."

Explicitly **deferred, not done** (Scope): the other 20 TPC-H queries (they need JOIN execution wired into the executor, ADR 0012/0014), a DECIMAL type (ADR 0010/0011), subqueries, and the rest of SQL; **parallel query execution** — the SF1 numbers above are **serial**, because `query()` still aggregates serially (ADR 0015); and **x86/AVX2 numbers**, which come from CI. Every number in this ADR is **Apple M3 Pro / NEON**.

The artifacts: the SF1 harness is `bench/tpch_runner.cpp` (loads the projected `lineitem` columns from CSV, runs Q1/Q6 via `query()`, validates against baked DuckDB oracle values, times them); the committed CI regression test is `tests/test_tpch.cpp`; the recorded run lives in `docs/BENCHMARKS.md`.

## Context

P9 is the credential the whole project was built to earn: **run TPC-H end-to-end, validate against DuckDB as the oracle, and measure honestly.** The thesis of Strata is "a mini-DuckDB, validated against DuckDB," and honesty is the entire thesis — no figure is fabricated, rounded, or cherry-picked; every number is from an actual run; being slower than DuckDB is fine *if explained*.

The hard constraint is ADR 0014: the executor lowers and runs **single-table plans** end-to-end. The hash-join operator exists and is tested in isolation (ADR 0012), and the logical-plan IR plus a predicate-pushdown-through-join rule exist — but **join execution is not wired into the executor** (parser join syntax + executor lowering are the missing pieces). So the set of TPC-H queries Strata can run *today* is precisely the set that has no join.

That set, canonically, is **Q1 and Q6** — and they are not a consolation prize, they are the two most-cited single-table TPC-H benchmarks:

- **Q1** is the canonical **aggregation and expression-throughput** benchmark: a low-cardinality `GROUP BY` (`l_returnflag, l_linestatus`) with several `SUM`/`AVG` over per-row arithmetic (`l_extendedprice * (1 - l_discount)`, `... * (1 + l_tax)`), plus `ORDER BY`. It also carries a `WHERE l_shipdate <= date '1998-09-02'` predicate, but that filter is **high-selectivity** — it removes only the trailing ~3% of shipdates — so the query's headline cost is the grouped aggregation, not the filter. It stresses exactly the hash-aggregate (ADR 0010/0011) and vectorized expression-eval (ADR 0009) paths.
- **Q6** is the canonical **scan/filter** benchmark: a conjunctive range filter over `l_shipdate`, `l_discount`, `l_quantity` (`l_shipdate` in 1994, `l_discount BETWEEN 0.05 AND 0.07`, `l_quantity < 24`), summing `l_extendedprice * l_discount`. It stresses the scan, the predicate evaluation, and a single global `SUM`, and — unlike Q1's high-selectivity filter — it discards the large majority of rows.

Strata loads only the **7 projected columns** these two queries reference (`l_quantity, l_extendedprice, l_discount, l_tax, l_returnflag, l_linestatus, l_shipdate`) from a CSV into an in-memory `ColumnarTable` (ADR 0007); load time is ~1.6 s, and is **not** counted in the query timings.

**The data and the oracle.** The dataset is DuckDB v1.5.3's `tpch` extension, `CALL dbgen(sf=1)` — `lineitem` = 6,001,215 rows. DuckDB is also the **oracle** in two distinct senses, and the ADR keeps them separate: (1) the **correctness oracle** — DuckDB's results are ground truth; and (2) the **performance baseline** — DuckDB's wall-clock is what we measure the gap against.

**The decimal problem.** TPC-H's monetary and quantity columns are `DECIMAL(15,2)`. Strata has **no DECIMAL type** (ADR 0010/0011, a deliberate prior decision — Strata's types are int32/int64/double/date/varchar). So Strata loads those columns as **`double`**. That is fine for an analytical engine but it means a naive comparison against DuckDB's *native* decimal results would conflate two unrelated things: "is Strata's computation correct?" and "double vs decimal representation." The methodology below exists to disentangle exactly that.

## Decision

### 1. DuckDB is the oracle for both correctness and the performance baseline

Correctness: a query is "validated" iff its result matches DuckDB's result for the same query on the same data within a stated tolerance. Performance: the gap is always reported against DuckDB on the same machine, at both `threads=1` and DuckDB's multi-threaded default, so the comparison is honest about DuckDB's own configuration rather than quietly comparing serial-Strata to serial-DuckDB only.

### 2. Validate double-against-double; report decimal-vs-double as a separate, named delta

Because Strata has no DECIMAL, we validate **apples-to-apples in the double domain**: we run the oracle queries in DuckDB with the relevant columns **cast to `DOUBLE`**, and compare Strata's `double` result against DuckDB's `double` result within a tight **relative** tolerance. This isolates the question that actually matters for Strata's correctness — *"does Strata compute the same arithmetic?"* — from the orthogonal question of *"decimal vs double representation,"* which is a property of the type choice (ADR 0010/0011), not of the computation.

**Q6 makes the two domains concrete:**

| Quantity | Value |
|---|---|
| Strata (double) | `123141078.22829895` |
| DuckDB **double-domain** oracle | `123141078.2282996` |
| DuckDB **native-decimal** | `123141078.2283` |

Strata matches the double-domain oracle to **~13 significant figures**. The residual difference between the two double results is **summation order, not error**: IEEE-754 addition is **non-associative**, so two correct engines that add 6,001,215 products in different orders land on different low-order bits — both compute the same mathematical quantity in double. (This is the same FP-order caveat ADR 0011 records for `SUM(double)`, and the same one ADR 0015 records for cross-thread merge order.) The native-decimal value (`...2283`) differs from both doubles by a **low-digit representational** difference — that is decimal-vs-double, a consequence of the no-DECIMAL decision, and we report it as such rather than treating it as a discrepancy to be explained away.

**Q1 is validated the same way, group-by-group, in relative tolerance.** All 4 group rows match the double-domain oracle within tight relative tolerance; the largest aggregate illustrates it — `(A,F) sum_charge`: Strata `55909065222.8256` vs DuckDB `55909065222.8275`, **relative diff ~3e-14** — again pure double summation order, not error.

### 3. Two validation runs, by design

- **Committed regression test (`tests/test_tpch.cpp`).** A **12-row in-code `lineitem`** whose expected Q1/Q6 values were produced by **DuckDB in the double domain** on that exact 12-row dataset and baked in as literals. CI re-validates Strata's Q1/Q6 against DuckDB-derived ground truth **without needing DuckDB present** — it is part of the 141-test suite that is green under ASan/UBSan, release, and TSan. This is "validated against DuckDB" turned into a permanent, hermetic CI gate.
- **SF1 harness run (`bench/tpch_runner.cpp`, recorded in `docs/BENCHMARKS.md`).** The full 6,001,215-row validation and timing. This is the run that produces the numbers in this ADR.

## The gap, measured and explained

All timings are **median of 7** for Strata, on the **Apple M3 Pro** (5 P + 6 E cores, 11 total), `release` preset. DuckDB on its **native** `lineitem` (decimal), same machine. Strata's `query()` path is **serial**.

| Query | Strata (serial) | DuckDB, 1 thread | DuckDB, default (11 threads) | Strata vs DuckDB-1T | Strata vs DuckDB-default |
|---|---:|---:|---:|---:|---:|
| **Q6** (scan/filter) | 141 ms | ~21 ms | ~7 ms | **~6.7× slower** | **~20× slower** |
| **Q1** (group-by/expr) | 658 ms | ~232 ms | ~37 ms | **~2.8× slower** | **~18× slower** |

*Footnote on the table:* DuckDB's timings are measured on its **native-decimal** `lineitem`, while Strata operates on `double`. The double-vs-decimal difference is a **correctness note** (handled by the double-domain validation above), and a small slice of decimal-vs-double per-row work is mixed into DuckDB's wall-clock; it is **not** the material driver of the gap — multi-threading, scan-level statistics, and hash-table/compression engineering are.

The gap is **expected and acceptable** — the deliverable of P9 is *validated correctness plus a measured, explained gap*, not a competitive number. The explanation, grounded in the architecture:

**Per-query reasoning — why Q6's gap is bigger than Q1's.** The difference is about **how much of the input each engine can avoid touching**, and that turns on filter selectivity.

- **Q6 (filter-heavy, low-selectivity) shows the LARGER single-thread gap (~6.7×).** Q6's conjunctive range filter discards the large majority of rows. DuckDB's storage carries **min/max zone maps** — ADR 0007 contrasts this with Strata's flat 2048-row chunks that carry no block-level statistics — so DuckDB can push the predicates into the scan and **prune whole row groups whose `l_shipdate`/`l_discount`/`l_quantity` ranges cannot match**, then run a tight SIMD filter over the survivors. (This is the documented mechanism of DuckDB's storage, not an instrumented observation of this particular run.) **Strata has no such statistics, so it evaluates all predicates over all 6,001,215 rows with no skipping** — every row is touched. When the headline work is "decide which rows survive a highly selective conjunctive range filter," the engine that can *avoid touching most rows* wins by the most. That is precisely the structural reason Q6 shows Strata's widest single-thread gap.
- **Q1 (aggregation-heavy, high-selectivity filter) shows the SMALLER single-thread gap (~2.8×).** Q1 does carry a filter (`l_shipdate <= 1998-09-02`), and Strata evaluates that predicate over all 6,001,215 rows just as it does for Q6 — but the filter is **high-selectivity**: it removes only the trailing ~3% of shipdates. So unlike Q6 there is **almost nothing for DuckDB's zone map to skip**; both engines must take essentially every row through the **full grouped aggregation** — compute the per-row arithmetic and fold it into ~4 groups. That makes the comparison an **aggregation-vs-aggregation** story, where the dominant cost is work neither engine can elide, and Strata's vectorized hash-aggregate (ADR 0010/0011) plus vectorized expression eval (ADR 0009) is **relatively competitive**. A ~2.8× single-thread gap on the canonical aggregation query is the honest "Strata's compute path is reasonable" data point.

**General causes (attributed to DuckDB plainly, not hand-waved).** Beyond the per-query structure above, DuckDB is faster for reasons that are real engineering, and we name them:

1. **Default multi-threading — the single biggest factor against DuckDB's default.** DuckDB runs multi-threaded by default; **Strata's `query()` path is serial** (ADR 0015's parallel layer is not wired into it). This alone accounts for most of the jump from the ~3–7× single-thread gaps to the ~18–20× default-config gaps.
2. **Adaptive / dictionary compression** of the stored columns (Strata stores uncompressed columns, ADR 0007).
3. **More optimized hash tables** for aggregation than Strata's open-addressing table (ADR 0010).
4. **Expression rewriting** in the optimizer (Strata's optimizer does predicate + projection pushdown only, ADR 0014).
5. **Years of focused engineering** on exactly these two query shapes.

The honest framing: **being slower is expected and fine.** Strata is a study engine validated against the production oracle, not a competitor to it.

## The parallel connection (honest)

The morsel-driven parallel layer (ADR 0015) **does run on real `lineitem`**, and it scales: a **Q1-shaped** `GROUP BY l_returnflag, l_linestatus` with `sum(l_extendedprice) + sum(l_quantity) + count(*)`, driven through `ParallelAggregate`, goes **211 ms (1 thread) → 25 ms (11 threads) = 8.45×** (consistent with the standalone scaling curve in `BENCHMARKS.md`: sub-linear, ~8× at 11 threads, the asymmetric-core behavior ADR 0015 predicts).

But this is the **aggregation phase only** — **no `WHERE` filter, no per-morsel expression arguments.** `ParallelAggregate` aggregates table columns directly; it does not yet apply a filter or projection per morsel. So it is **not the full Q1**, and `query()` itself is **still serial**.

Critically, **the 8.45× and the serial 658 ms are measured on different workloads** — the 8.45× comes from a 211 ms aggregation-only kernel that omits Q1's filter and per-morsel expression args, so **no single multiplier maps 658 ms to a parallel full-Q1 figure**, and we deliberately do not put a number on it. The honest qualitative statement of the upside: *if* the full Q1 aggregation path were parallelized, Strata's serial ~658 ms would move **toward, but not into,** DuckDB's multi-threaded ~37 ms territory — **we do not claim it would match it**, and because we have not measured the full parallel Q1, we do not project a specific time for it. The clear next step, consistent with ADR 0015, is to **wire the parallel layer plus per-morsel filter/projection behind the planner**.

## Alternatives considered

**Fixed-point (scaled integer) decimal vs double for the TPC-H columns.** A real `DECIMAL(15,2)` (a scaled `int64`, exact two-place arithmetic) would let Strata match DuckDB's *native* output bit-for-bit and remove the decimal-vs-double delta entirely. **Rejected for P9** (it is a deferred type, not a P9 decision): introducing DECIMAL touches the type system, every arithmetic kernel, overflow policy, and `AVG`'s rounding semantics — a large piece of work that ADR 0010/0011 deliberately deferred. **Double-against-double validation makes the absence of DECIMAL a non-blocker for correctness:** it cleanly separates "is the computation right" (validated) from "decimal vs double representation" (a named, small delta). DECIMAL is recorded as future work, not papered over.

**The full 22-query suite now vs the runnable single-table subset.** Running all 22 would require wiring join execution into the executor + parser join syntax (ADR 0012/0014), and stubbing or faking the join queries we cannot actually run would violate the honesty thesis. **Rejected:** we run only what executes correctly end-to-end and **say so prominently** — "the two single-table TPC-H queries," with the other 20 explicitly deferred behind named, missing capabilities. A smaller honest claim beats a larger fabricated one; that is the entire point of the project.

**Generating Parquet vs CSV as the SF1 input.** Parquet would be smaller, faster to load, and carry column statistics (which could even narrow the Q6 zonemap gap). **Rejected for P9:** Strata's loader is the delimited-CSV loader (ADR 0007); CSV is the format the loader actually supports, the load is a one-time ~1.6 s cost excluded from query timings, and adding a Parquet reader is scope unrelated to "validate Q1/Q6 against DuckDB." Using the format we actually have keeps the harness honest and reproducible.

## Consequences

- **A defensible, validated P9 deliverable:** Strata runs the two single-table TPC-H queries end-to-end and matches the DuckDB double-domain oracle (~13 sig figs on Q6; relative diff ~3e-14 on Q1's aggregates), with a permanent, DuckDB-free CI regression test (`tests/test_tpch.cpp`, part of 141 green tests under ASan/UBSan/release/TSan) plus the SF1 harness run (`bench/tpch_runner.cpp`, recorded in `docs/BENCHMARKS.md`).
- **The gap is on the record, with causes, not hidden:** ~6.7× (Q6) / ~2.8× (Q1) vs single-threaded DuckDB; ~20× / ~18× vs DuckDB's multi-threaded default. The per-query asymmetry — Q6's larger gap = its low-selectivity filter lets DuckDB's zone maps skip most row groups while Strata touches all 6M rows; Q1's smaller gap = its high-selectivity filter leaves almost nothing to skip, so it is an aggregation-vs-aggregation comparison where Strata's vectorized agg is competitive — is the centerpiece finding.
- **The single biggest lever is named:** Strata's serial `query()` vs DuckDB's default multi-threading. The parallel layer exists and scales 8.45× on a Q1-shaped aggregation over real `lineitem`, but is not wired into `query()` and does not cover the filter/per-morsel-expression work — so the path to closing *part* of the gap is concrete (ADR 0015) and not overclaimed.
- **Known, prominently-stated limits:** no DECIMAL (so a named decimal-vs-double delta); no join execution in the executor (so 20/22 queries deferred); serial query path; all numbers M3 Pro / NEON, with x86/AVX2 to come from CI.
- **`docs/BENCHMARKS.md`'s P9 sections (TPC-H SF1 vs DuckDB; Gap analysis) are filled by this work** — the methodology those placeholders promised is now an actual recorded run. (The harness's Q6 validation `printf` label should print the double oracle `123141078.2282996` it actually compares against, rather than the native-decimal `123141078.2283`, to match the value being checked.)

## How to defend this at a whiteboard

- "**The runnable subset is honest, not a dodge.** My executor runs single-table plans (ADR 0014 — the join operator exists and is tested, but isn't wired into the executor yet), so the TPC-H queries I can run end-to-end are the **single-table ones: Q1 and Q6**, and those happen to be the two most-cited single-table TPC-H benchmarks — Q1 the canonical aggregation/expression query, Q6 the canonical scan/filter. So I say 'I ran the two single-table TPC-H queries, correctly,' never 'I ran TPC-H.'"
- "**DuckDB is my oracle twice over** — ground truth for correctness *and* the performance baseline. I have no DECIMAL type, so TPC-H's `DECIMAL(15,2)` loads as `double`; to validate apples-to-apples I run the oracle in DuckDB with those columns **cast to DOUBLE** and compare in **relative tolerance**. That isolates 'is my computation correct' from 'decimal vs double representation.' On Q6: I'm `...22829895`, DuckDB-double is `...2282996` — match to ~13 sig figs, and the residual is **summation order**, because IEEE add is **non-associative**; native-decimal is `...2283`, which is a representational delta from the type choice, and I report it as that, not as an error."
- "**I have two validation runs.** A committed 12-row in-code `lineitem` with DuckDB-baked expected values, so CI re-validates against DuckDB **without DuckDB installed** — it's in my 141-test suite, green under ASan/UBSan/release/TSan. And the SF1 harness run on 6,001,215 rows recorded in BENCHMARKS.md."
- "**The gap, and I lead with it.** Serial Strata is ~6.7× (Q6) / ~2.8× (Q1) slower than 1-thread DuckDB, ~20× / ~18× slower than DuckDB's 11-thread default. **Both queries have a WHERE clause, but the selectivities differ and that's the whole story.** Q6's filter is low-selectivity — it throws away most rows — and DuckDB's zone maps (ADR 0007 says it has min/max stats per row group and I have flat 2048-row chunks with none) let it skip whole row groups, while I evaluate every predicate over all 6M rows; that's why Q6 is my widest gap. Q1's filter (`l_shipdate <= 1998-09-02`) is high-selectivity — it removes only ~3% — so there's almost nothing for DuckDB to skip and both of us push essentially every row through the grouped aggregation; that's an agg-vs-agg comparison where my vectorized hash-agg plus expression eval is competitive, hence the smaller gap. The biggest general factor is that **DuckDB is multi-threaded by default and my `query()` is serial**; the rest is compression, better hash tables, expression rewriting, and years of engineering. Being slower is the expected, fine outcome — the deliverable is **validated correctness plus a measured, explained gap.**"
- "**The parallel upside, stated honestly.** My morsel layer scales **8.45× (211→25 ms)** on a *Q1-shaped* aggregation over real `lineitem` — but that's aggregation only, **no filter, no per-morsel expression args**, and that 211 ms is a different workload from the serial 658 ms full Q1, so I can't just divide 658 by 8.45. Wiring the parallel layer plus per-morsel filter/projection behind the planner is the clear next step, and it would move me *toward* DuckDB's multi-threaded territory — but I do **not** claim it'd reach 37 ms, and I won't put a number on the full parallel Q1 because I haven't measured it."
- "**What's deferred, plainly:** the other 20 queries need join execution wired in (the operator's done, ADR 0012; lowering + parser join syntax aren't), plus a DECIMAL type, subqueries, the rest of SQL. And every number here is **M3 Pro / NEON** — x86/AVX2 comes from CI."
