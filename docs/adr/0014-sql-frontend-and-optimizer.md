# 0014. SQL front-end: hand-written parser, logical plan IR, and a rule-based optimizer

## Status

Accepted, as-built for P7. (Single-table parse-and-execute is end-to-end; JOIN execution is deferred to P9. The `Join` logical node and an *inner-join* predicate-pushdown rule are built and tested at the IR level in P7, so the IR and optimizer are ahead of the executor for the inner-join case only.)

## Context

Through P6 (decisions 0006–0013) Strata is a *physical* engine: it has a `ColumnarTable` data plane, a push-based Source/Sink/Pipeline execution model with sinks as pipeline breakers, SIMD comparison/arithmetic kernels, a 3-valued `ExpressionExecutor`, hash aggregation, hash join, and sort/limit/top-n. What it lacks is a way to *describe* a query: there is no text-facing surface and no operator graph above the hand-wired physical operators. P7 adds the front-end — the path from a SQL string to a running push pipeline whose result is validated against DuckDB, our oracle.

This requires four layers, and the central engineering question of P7 is how to draw the lines between them:

1. **Parser** — SQL text to an abstract syntax tree.
2. **Logical plan IR** — a relational-algebra operator tree, decoupled from physical operators, carrying expressions that a **binder** resolves against a **catalog** of registered tables.
3. **Optimizer** — semantics-preserving rewrites over the logical tree.
4. **Physical planning + execution** — lower the optimized logical plan to physical operators and run them on the push pipeline.

The build spec's *primary preference* for the parser was to integrate the third-party `hyrise/sql-parser` (a C++ SQL parser), with a hand-written parser as the explicitly-sanctioned fallback. P7 takes the fallback, deliberately, and this ADR documents that choice honestly along with everything built on top of it.

The honesty constraint of the whole project applies with force here: the front-end is where overclaiming is easiest ("we support SQL") and least true. This ADR states exactly which SQL subset is accepted, which rules are heuristic and which optimizations are absent, where a rewrite is sound only under a stated restriction, and what is end-to-end now versus deferred.

## Decision

### Parser: hand-written recursive-descent + tokenizer over the executable subset

We hand-wrote a tokenizer and a recursive-descent parser covering precisely the SQL subset the engine can run, rather than integrating `hyrise/sql-parser`. This is a deliberate deviation from the spec's stated primary preference, justified by four points:

1. **Exact control of the grammar.** The parser accepts *only* constructs the engine can execute, so there is never a "parses but cannot execute" gap — the acceptance boundary of the front-end and the capability boundary of the engine are the same line by construction. A general-purpose parser accepts a far larger language than Strata implements, and every accepted-but-unimplemented construct becomes a runtime "unsupported" error we must detect and reject *after* parsing.

2. **No third-party build/AST-translation surface in our pinned, two-compiler CI.** Decision 0002 commits us to `-Wpedantic -Werror` on first-party code under both clang (22+) and gcc (14), with deps pinned via `find_package`/`FetchContent`. As of 2026-06, upstream `hyrise/sql-parser` (github.com/hyrise/sql-parser) is **Makefile-based**: its repo root ships a `Makefile` and **no first-class `CMakeLists.txt`**; it generates its parser/lexer with **bison and flex** (`src/parser/bison_parser.y`, `src/parser/flex_lexer.l`) and builds a `libsqlparser.so` via `make parser` / `make library` (its dev-docs describe only `make` targets). Third-party forks exist that add CMake (e.g. TU-Dortmund's HyriseSQLParser), but upstream does not; that distinction is itself a maintenance hazard. Adopting upstream would mean either driving its Makefile + bison/flex toolchain from our CMake graph or vendoring its generated sources, *and* writing a translation layer from its large general-purpose AST into our IR. That is real integration and translation cost, absorbed into the strictest part of our CI, for a language we only partially implement.

3. **A library buys no correctness we cannot already validate.** DuckDB is the oracle for *both* parse acceptance and result correctness regardless of which parser we use. We confirm against DuckDB that a query is valid SQL and that our result matches; the parser's provenance changes nothing about that loop. A third-party parser would give broader *acceptance*, not better *correctness*.

4. **The parser is the most isolated, replaceable layer.** The IR, binder, optimizer, and executor are parser-agnostic — they consume the AST/IR, not SQL text. Swapping in `hyrise/sql-parser` later to broaden coverage is a contained change: add a translation pass from its AST to our unbound IR; nothing downstream moves.

The honest cost is **limited SQL coverage** — only the subset below, hand-maintained. We accept that cost as the explicit price of the fallback.

**Supported subset (P7):**
- `SELECT` list: column references, `*`, arithmetic, comparisons, `AND`/`OR`/`NOT`, and the five aggregates (`COUNT`, `SUM`, `MIN`, `MAX`, `AVG`) each with an optional `AS` alias.
- `FROM` a single table.
- `WHERE`, `GROUP BY`, `ORDER BY` (per-key `ASC`/`DESC`), `LIMIT`.
- Literals: integer, double, string, and `DATE 'YYYY-MM-DD'`.
- `BETWEEN` **desugars** in the parser to `>= AND <=`, so the IR and executor never see a `BETWEEN` node.

Single-table queries parse and execute end-to-end in P7. **JOIN parsing and execution are scoped to P9.** The `Join` logical node and an *inner-join* predicate-pushdown rule are nonetheless built and tested at the IR level in P7 so the IR and optimizer lead the executor for that case; outer-join semantics and join ordering are not addressed (see the optimizer section and Tradeoffs).

### Logical plan IR with a binder over a catalog

The logical plan is a tree of relational-algebra operators — `Get`, `Filter`, `Project`, `Aggregate`, `Join`, `Order`, `Limit` — each carrying expressions. The `Join` node currently models **inner join only**. Expressions are produced **unbound** (column references are names) and a **binder** pass turns them **bound** by resolving each name against a **catalog** of registered `ColumnarTable`s, assigning every column reference its physical index and type.

This logical algebra is **decoupled from the physical operators** of 0006–0013. The optimizer rewrites logical nodes; only physical planning maps them to `Scan`/`Filter`/`Project`/`HashAggregate`/`Sort`/`TopN`/`Limit`. The separation is what makes both the optimizer and the executor independently testable. This logical-vs-physical operator-IR split — distinct logical operators that transformation rules rewrite, lowered separately to physical operators — is the design codified by Graefe's **Volcano (1994) / Cascades (1995)** optimizer frameworks; it sits on the older relational-algebra foundation but is not the contribution of System R.

### Rule-based optimizer (heuristic, not cost-based)

The optimizer applies semantics-preserving heuristic rewrites. It is **rule-based, not cost-based**: rules fire because they are *generally* good, not because a cost model scored alternatives.

- **Constant folding.** Constant sub-expressions are evaluated at bind/optimize time (e.g. `1 + 2` to `3`), so the per-tuple `ExpressionExecutor` never re-evaluates an expression whose value is fixed. The folder uses the **identical arithmetic, 3-valued-logic, overflow, and division semantics as the `ExpressionExecutor`** (0008/0009), so plan-time folding is observationally equivalent to per-tuple evaluation; cases whose behavior is row-position-independent but error-prone at plan time (e.g. integer overflow, division by zero) are **left unfolded and deferred to runtime** rather than risk a plan-time/run-time divergence. *Why it helps:* removes work from the hot path. It is intentionally **shallow** — it folds literal-only sub-trees; it does not do algebraic simplification (`x*1`, `x+0`), common-subexpression elimination, or strength reduction.

- **Predicate pushdown (inner join only).** A conjunctive `WHERE` is split on `AND` into individual conjuncts, and each conjunct is pushed as far down the tree as it can legally go: below `Project`, and **through an inner `Join`** to the side (or sides) whose columns it exclusively references. *Soundness note:* pushing a single-side conjunct below a join is semantics-preserving for **inner joins** (and across the **preserved** side of an outer join); it is **not** sound to push a filter onto the **null-supplying** side of a LEFT/RIGHT/FULL OUTER join, because the filter would apply *before* null-extended rows are generated and could turn an outer join into a de-facto inner join or drop rows that should survive with NULLs. Strata's IR currently models only inner joins, so the rule is scoped accordingly; outer-join pushdown (and the NULL-rejecting-predicate special case that can legally simplify an outer join to inner) is future work. *Why it helps:* filtering early shrinks the intermediate cardinality every operator above must process — fewer rows hashed, joined, sorted. The inner-join case "filter moved below the join" is asserted directly by a plan-rewrite test (exercised in P7 even though join *execution* lands in P9).

- **Projection pushdown.** Top-down, we compute for each `Get` the exact set of columns that anything above it references, and prune the scan to read only those columns. *Why it helps:* it lets the planner exploit the columnar format by telling the scan to touch only the bytes of referenced columns. The storage format already reads only the columns it is asked for; projection pushdown is the rewrite that *narrows that request* to the query's true column footprint, so the scan never materializes columns the query does not use. It is a natural, high-value rewrite for a column store and complements predicate pushdown — projection pushdown trims column *width*, predicate pushdown trims row *cardinality*. We do not claim a ranking between them: which dominates is query-dependent and we have not benchmarked it.

**Cost-based optimization is deferred** as a stretch. It is not built, because it presupposes machinery Strata does not yet have: per-column **cardinality statistics** (distinct counts, value distributions) and a **cost model** to compare candidate plans. Absent both, the optimizer cannot make *any* cost-dependent decision — not only the famous one (**join reordering**, e.g. Selinger et al., System R, SIGMOD 1979, dynamic programming over left-deep trees), but also: choosing hash-join **build vs probe sides**, **ordering predicate evaluation by selectivity/cost** (a cheap comparison before an expensive one), and deciding whether a given pushdown actually *pays off* rather than applying it unconditionally. The honest position is therefore to ship the heuristic rules now — sound under their stated restrictions — and defer every cost-based decision until statistics and a cost model exist.

### Physical planning and push-pipeline execution

Physical planning translates the optimized logical tree into physical operators (`Scan`, `Filter`, `Project`, `HashAggregate`, `Sort`, `TopN`, `Limit`) and runs them on the push model of decision 0001, where **sinks are pipeline breakers and the executor holds the loop**. These physical operators — including the **selection-vector `Filter`** (it marks surviving rows rather than copying) and the **gather `Project`** — are the existing operators from decisions 0008/0009 and 0010–0013; P7 adds only the logical-to-physical *lowering*, not the operators themselves.

**ORDER BY lowering:** `ORDER BY` immediately followed by `LIMIT` lowers to the `TopN` operator (a single breaker, decision 0013); a bare `ORDER BY` lowers to `Sort`. This is a structural rewrite, not a cost-based choice.

Execution **decomposes at pipeline breakers** (`Aggregate`, `Sort`/`TopN`) into a chain of sinks, each feeding the next on `Finalize`. The scan-side streaming operators drive the first sink: `Filter` produces its selection vector and `Project` performs its gather, and together they push batches into the breaker's sink. When that breaker finalizes, its output becomes the input that drives the next stage. A query of the form scan → filter → project → aggregate → sort → limit has **two pipeline breakers** (the aggregate and the sort), which split it into **three pipeline segments**: `[scan → filter → project → aggregate-sink]`, then `[aggregate-output → sort-sink]`, then `[sort-output → limit]` — wired so each breaker's `Finalize` launches the downstream segment.

The end-to-end entry point is `query(sql, catalog) -> result`: parse, bind, optimize, physical-plan, execute. It is **validated against DuckDB on single-table queries** (TPC-H Q1- and Q6-style: both are single-table over `lineitem` — filter + aggregate + group/order). **Oracle comparison methodology:** result rows are compared **order-normalized** (both sides sorted/canonicalized before comparison, except where an `ORDER BY` makes ordering itself part of the contract, in which case the produced order is checked directly); integer and `DATE` values are compared **exactly**, while floating-point aggregates (`AVG`, and `SUM` over doubles in Q1) are compared with a **numeric tolerance** rather than bit-exactly, since accumulation order legitimately differs from DuckDB's. **JOIN executor wiring** — build the child pipeline, then probe — is **completed in P9**; in P7 the join exists as an inner-join logical node and an inner-join optimizer rule, not as a runnable physical operator.

## Alternatives Considered

- **Integrate `hyrise/sql-parser` (the spec's primary preference).** Rejected for P7. As of 2026-06, upstream is Makefile-based with bison/flex grammar generation and no first-class CMake (building `libsqlparser.so` via `make`), so it would push a foreign toolchain and a generated `.so` into our pinned, dual-compiler, `-Werror` CMake build, plus a translation pass from its large general-purpose AST into our IR. It buys broader *acceptance* but no *correctness* (DuckDB validates either way), and because the parser is the most isolated layer, deferring its adoption costs us nothing structurally. We keep it as the documented upgrade path for widening SQL coverage.

- **A parser generator of our own (bison/flex or a PEG/ANTLR grammar).** Rejected. It reintroduces a code-generation step and a build-tool dependency into CI for a grammar small enough that hand-written recursive descent is comparable in size, easier to read at a whiteboard, and trivially `-Wpedantic`-clean. Generated parsers also tend to over-accept, recreating the "parses but cannot execute" gap we want to avoid.

- **Skip the logical IR; lower the AST straight to physical operators.** Rejected. Fusing logical and physical kills the layer the optimizer rewrites and forces every rule to reason about physical concerns (selection vectors, pipeline breakers). The logical/physical split is what makes the optimizer and executor independently testable; it is the standard modern structure codified by Volcano/Cascades (Graefe, 1994/1995).

- **Cost-based optimization now (join-order DP + a cost model; Selinger et al., System R, 1979).** Rejected for P7 as premature, not wrong. Without cardinality statistics and a cost model it has no inputs; building those is a project of its own. We ship safe heuristic rules now and keep cost-based optimization — join ordering, build-side selection, selectivity-ordered predicates — as the documented stretch.

- **Eager (pull/Volcano-style iterator) execution for the front-end.** Rejected — it contradicts decision 0001's push model. (Note: this concerns Volcano's *iterator execution* model, separate from the Volcano/Cascades *optimizer* framework cited above for the logical/physical split.) Physical planning targets the existing push operators; nothing in P7 reopens that decision.

## Consequences

**Wins**
- **No gap between accepted and executable SQL.** The grammar *is* the capability set; an unsupported query is rejected at parse time with a clear error, not deep in execution.
- **CI stays clean and self-contained.** No third-party parser toolchain (bison/flex/Makefile) or generated `.so` enters the pinned, two-compiler, `-Werror` build; the front-end is all first-party, `-Wpedantic`-clean C++23.
- **Clean layering.** Parser, IR + binder/catalog, optimizer, and physical executor are independently testable. Optimizer rules are checked as plan-rewrite assertions (notably "filter moved below the join" for the inner-join case) without running a query.
- **Columnar format exploited at plan time.** Projection pushdown narrows each scan's request to the query's true column footprint, so scans never materialize unreferenced columns.
- **Lower cardinality earlier.** Predicate pushdown shrinks intermediate results before they reach hashing, joining, and sorting.
- **Validated where it runs.** Single-table `query(sql, catalog)` is checked against DuckDB end-to-end, with order-normalized comparison and numeric tolerance for float aggregates.
- **Cheap upgrade path.** Because the parser is the most isolated layer, adopting `hyrise/sql-parser` later (or growing the hand-written grammar) is a contained change behind a stable IR.

**Tradeoffs (stated honestly)**
- **Limited SQL subset.** Only the constructs listed are accepted; broadening coverage is manual grammar work. This is the explicit price of the fallback and a deliberate deviation from the spec's primary preference.
- **Rule-based, not cost-based — and the gap is broader than join ordering.** With no statistics and no cost model the optimizer cannot make *any* cost-dependent decision: it cannot reorder joins, cannot choose hash-join build/probe sides, cannot order predicate evaluation by selectivity/cost, and applies pushdown unconditionally even where it might not pay off. Join reordering is the flagship missing capability, not the only one.
- **No statistics.** No cardinality estimates, histograms, or distinct counts — hence no cost model and no cost-based decisions of any kind.
- **Shallow constant folding.** Only literal-only sub-expressions fold (using the executor's exact semantics, with overflow/div-by-zero left to runtime); no algebraic identities, CSE, or strength reduction.
- **Inner joins only; outer-join pushdown unhandled.** The IR models inner join only, and the predicate-pushdown rule is sound under that restriction. Pushing a filter onto the null-supplying side of an outer join is unsound and is deliberately not done; a single green inner-join plan-rewrite test does not establish general join correctness, so we do not claim a "join-ready" optimizer beyond the inner-join case.
- **Simple binder.** Name/index/type resolution against the catalog only. Coercion is limited: mixed numeric `int`/`double` operands are coerced to a common type for arithmetic and comparison, but there is **no string↔numeric and no string↔date coercion** — those are errors, not silently widened. Because DuckDB coerces more liberally, Strata will *reject* some queries DuckDB accepts; this is an intentional strictness choice, documented here so it is not mistaken for an oracle mismatch.
- **Join execution deferred to P9.** The inner-`Join` logical node and the inner-join predicate-pushdown rule exist and are tested at the IR level, but the build-then-probe physical wiring is not runnable in P7; end-to-end validation in P7 is single-table only.

## How to defend this at a whiteboard

- **"Why hand-write the parser instead of using the library the spec preferred?"** Four reasons: the grammar exactly equals what we can execute (no parses-but-can't-run gap); we avoid pulling a Makefile/bison/flex toolchain and a `libsqlparser.so` plus an AST-translation layer into a pinned, two-compiler, `-Werror` CMake build; DuckDB is the correctness oracle either way so a library buys acceptance, not correctness; and the parser is the most isolated layer, so swapping the library in later is contained. I name the deviation up front and name its cost: a smaller SQL subset.
- **Know the library's build facts cold.** As of mid-2026, upstream `hyrise/sql-parser` is Makefile-based with bison/flex, ships no first-class CMake, builds `libsqlparser.so`. Third-party forks (e.g. TU-Dortmund) add CMake; upstream doesn't. That is precisely the integration surface I chose not to absorb.
- **Logical vs physical, and who to credit.** Logical = relational algebra (`Get`/`Filter`/`Project`/`Aggregate`/`Join`/`Order`/`Limit`); physical = the push operators. The split is what the optimizer rewrites against and what makes optimizer and executor independently testable — that operator-IR separation is Volcano/Cascades (Graefe, 1994/1995), *not* System R. I credit Selinger/System R (1979) only for cost-based join-order DP.
- **Binder + catalog in one line.** Unbound expressions carry names; the binder resolves them against registered tables, stamping each column reference with index and type; coercion is numeric-only.
- **Three rules, each with its *why* — and one with a guard.** Constant folding removes hot-path work, uses the executor's exact 3VL/overflow semantics, and is shallow. Predicate pushdown cuts row cardinality before joins/aggregates/sorts — and I'll volunteer the outer-join caveat: pushing to the null-supplying side is unsound, so the rule is inner-join-only, which is all our IR models today. Projection pushdown lets the planner narrow the scan's column request to exploit the columnar format; it complements predicate pushdown (width vs cardinality) and I will *not* rank them without a benchmark.
- **Heuristic, not cost-based — and I'll say what that blocks.** The rules are sound under their stated restrictions. What I *can't* do without stats + a cost model: reorder joins, pick hash-join build/probe sides, order predicates by selectivity, or know whether a pushdown pays off. That's the documented stretch (join-order DP a la Selinger), not built.
- **Execution model and the pipeline count.** Push pipeline; sinks are pipeline breakers. A scan→filter→project→aggregate→sort→limit query has *two* breakers (aggregate, sort) and therefore *three* pipeline segments, each launched by the prior breaker's `Finalize`. `Filter` emits a selection vector and `Project` gathers (both inherited from 0008/0009). `ORDER BY`+`LIMIT` lowers to `TopN`; bare `ORDER BY` to `Sort`.
- **How I validate against DuckDB.** Order-normalized row comparison (except when `ORDER BY` makes order part of the answer), exact for int/date, numeric tolerance for float aggregates like `AVG`/double `SUM`. That's why Q1's averages match despite different accumulation order.
- **What's real vs deferred.** `query(sql, catalog)` runs single-table queries end-to-end, validated against DuckDB (Q1/Q6-style). The inner-join *logical node + rewrite rule* are tested in P7; join *execution* (build then probe) and outer-join semantics land later. I will not claim more than that.
