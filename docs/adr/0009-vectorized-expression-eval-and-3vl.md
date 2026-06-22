# 0009. Vectorized expression evaluation and three-valued NULL logic

## Status

Accepted.

## Context

The previous P3 work established the SIMD kernel layer: per-batch, lane-parallel comparison and arithmetic over flat numeric `Vector`s, dispatched through Highway (`foreach_target.h` + `HWY_DYNAMIC_DISPATCH` + `HWY_EXPORT`). Those kernels are leaf operations. A query predicate like `o_totalprice > 100 AND o_orderstatus <> 'F'` is not a single kernel call — it is a *tree* of operations, mixing numeric comparison, string comparison, and Boolean connectives, over columns that may contain NULLs.

This ADR specifies the layer that sits above those kernels and below the operators: the **ExpressionExecutor**, which evaluates an `Expression` tree over a `DataChunk`, and the **Filter** and **Project** operators that consume its results. (The numeric-kernel semantics — Highway dispatch, lane behavior, mask narrowing, wrapping arithmetic — are stated as-built here in the Decision; they are not factored into a separate record. The SIMD *ceiling* — what SIMD does and does not accelerate — is documented in `LIMITATIONS.md` §3 and §4 and ADR 0004, and is referenced rather than re-argued.)

The hard part is not the arithmetic. The hard part is correctness under SQL's three-valued logic (3VL), where the third truth value, `NULL`/unknown, propagates through strict functions but is *absorbed* by the Boolean connectives in a way that is not a simple mask operation.

SQL semantics we must honor exactly (validated against DuckDB):

- Strict scalar functions (arithmetic, comparison) return `NULL` if **any** input is `NULL`. `x > 5` where `x IS NULL` is `NULL` (unknown), **not** `FALSE`.
- `WHERE` admits a row only if the predicate evaluates to `TRUE`. `FALSE` and `NULL` are both rejected.
- The Boolean connectives `AND`/`OR` are **not** strict: `FALSE AND NULL = FALSE`, `TRUE OR NULL = TRUE`. `NULL` survives only when no operand is decisive.

Getting this wrong is silent data corruption that passes casual testing and fails the DuckDB diff on exactly the rows that contain NULLs — i.e., the rows that matter.

## Decision

### ExpressionExecutor: one dispatch per batch, per node

The `ExpressionExecutor` recursively evaluates an `Expression` tree against a `DataChunk`. Node types: `ColumnRef`, `Constant`, `Comparison` (`= <> < <= > >=`), `Arithmetic` (`+ - * /`), and the logical connectives `AND`, `OR`, `NOT`.

Evaluation is post-order: children first, then the node. Each node produces exactly one result `Vector` covering the full chunk. The dispatch discipline is **once per batch, per node** — a `Comparison` node makes a single `HWY_DYNAMIC_DISPATCH` call into the numeric kernel for the whole `DataChunk`. This amortizes the interpreter's per-node **function-call / virtual-dispatch and operand-setup overhead** over up to 2048 rows; it is the X100 vector-at-a-time thesis (Boncz, Zukowski, Nes, *MonetDB/X100: Hyper-Pipelining Query Execution*, CIDR 2005), whose contribution is amortizing per-tuple *interpretation* overhead generally, not narrowly eliminating one virtual call. (`HWY_DYNAMIC_DISPATCH` itself resolves the SIMD target *once* and caches the chosen function pointer, so beyond the first call this is ordinary indirect-call amortization, not repeated runtime feature detection.) There is no per-row dispatch in the numeric path.

- `ColumnRef` returns a view/reference to the column `Vector` in the chunk (no copy).
- `Constant` is **broadcast** to a flat `Vector` (see Tradeoffs).
- `Comparison` / `Arithmetic` over numeric types route to the SIMD kernels.
- Logical `AND`/`OR`/`NOT` and string comparison run as NULL-aware **scalar** loops.

The executor always evaluates over the **full chunk**. It does not itself apply a selection; row selection is the job of `Filter`. This keeps the kernels operating on dense, contiguous, aligned arrays — the regime in which they are fast.

#### Numeric kernel mechanics, as-built (inherited by every strict node)

These are properties of the kernel layer that the executor depends on and that a whiteboard answer must cover:

- **Mask production and narrowing.** A `Comparison` kernel produces a per-lane mask in the comparison's own type domain, which is then narrowed to a one-byte-per-row result via `DemoteTo(Rebind<uint8_t>, vec)` (verified: `int32 -> uint8` and `int64 -> uint8` narrowing both work in this toolchain). For `double` comparison, the `f64` mask is first routed into the **signed-integer domain via `RebindMask`**, then `DemoteTo` to `uint8` (verified). So "the comparison writes a result bit" is concretely: compare in-domain, rebind the mask if floating, demote to a `uint8` lane.
- **NEON lane-count reality (verified).** On the M3 dev machine, NEON is 128-bit: **4× `int32`** per vector but only **2× `int64`/`double`**. The x86_64 CI runner compiles the *same* Highway sources to AVX2 (256-bit), doubling both. This matters because TPC-H arithmetic is heavily `int64`/decimal and `double` (e.g. `l_extendedprice * (1 - l_discount)`) — exactly the 2-lane-wide types on NEON. The per-type speedup is therefore **width-dependent** and is measured *per type* in `docs/BENCHMARKS.md`; this ADR makes **no** blanket "arithmetic is uniformly the cheap/fast part" claim. Comparison and arithmetic are both branch-free elementwise ops; comparison additionally pays the `DemoteTo` narrowing above. Numbers, not adjectives, settle which wins where.
- **Wrapping integer arithmetic, kept UB-clean.** Integer arithmetic uses two's-complement **wrapping** semantics with no overflow check. The subtlety the honesty rule demands: signed-integer overflow is *undefined behavior* in C++ and would trip UBSan (`-fsanitize=signed-integer-overflow`) under the `asan-ubsan` correctness preset (ADR 0002). We stay UB-clean because the arithmetic goes through **Highway's vector ops**, whose integer add/sub/mul are defined-wrapping at the hardware/intrinsic level rather than a naive source-level signed `+` (and the `asan-ubsan` preset, which ADR 0002 charges with catching "UB in bit tricks," does not flag the intrinsic path). Wrapping is documented behavior, not a silent surprise — and it is *defined*, not UB, as built.

### Strict functions and NULL propagation (the ADR 0003 payoff)

For strict nodes (arithmetic, comparison) the implementation computes the value for **all** rows unconditionally — including rows where an operand is NULL. The value computed under a NULL operand is **garbage, and that is fine**, because it is then masked off: the result validity bitmask is set to the **bitwise AND of the operand validity masks**.

This is the direct dividend of ADR 0003's conventions:

- `1 = valid`, so `AND` is exactly NULL-propagation: a result lane is valid iff *every* input lane was valid.
- An unallocated mask means "all valid", giving the **O(1) all-valid fast path**: if all operand masks are all-valid, the result is all-valid and we skip mask computation entirely. This is the common case (most columns have no NULLs) and it costs nothing.

Computing-then-masking is *more* work per row than branching, but it is **branchless** and lane-parallel: the kernel never has to ask "is this lane NULL?" mid-loop, so the SIMD comparison/arithmetic stays straight-line. We pay one extra `AND` over two bitmasks per batch (skipped entirely on the fast path) to keep the inner loop branch-free.

**Walk-through — `x > 5`, `x IS NULL`:** the kernel compares the garbage payload in that lane against 5, narrows the mask to a `uint8` lane in the result value vector (per the narrowing mechanics above). Independently, the result validity mask = `validity(x)` — and *only* `validity(x)`. The literal `5` is a non-null constant, i.e. **all-valid (unallocated mask)** per ADR 0003, so ANDing the result against the constant's validity is a no-op and is *skipped on the fast path*; there is no per-row mask to AND against for a literal. `validity(x)` has a `0` in that lane, so the result lane is `0` = invalid = NULL. The value bit is present but unreadable; every consumer checks validity first. The row's predicate result is **NULL (unknown)**, never `FALSE`. The two-operand validity AND only does real work when *both* operands are nullable columns (e.g. `a > b` with both `a` and `b` nullable).

### Boolean result representation

The comparison/Boolean result `Vector` is a **flat `uint8` value array** (one byte per row, `1` = true / `0` = false) **plus the standard validity bitmask** (one bit per row, ADR 0003). The value is a byte per row — *not* a packed bit — because that is what `DemoteTo(... uint8 ...)` produces from the kernel and what the scalar 3VL loops read/write per row; it stays a FLAT vector kind (ADR 0005). The validity side is the usual bitmask. This is the layout the Filter rule below operates on, and it is named here so the rule is unambiguous.

### WHERE / Filter semantics: only TRUE passes

`Filter` evaluates its predicate expression to a result `Vector` (a `uint8` value array plus a validity bitmask) and builds/refines a **selection vector** of the row indices that pass. A row `i` passes iff:

```
valid(i)  AND  value(i) == 1
```

That is, the predicate must be both **not NULL** *and* **TRUE**. Concretely, for each candidate row the selection test ANDs the row's `value` byte (reduced to a truth bit) with its validity bit: a row that is NULL (`validity bit = 0`) is rejected regardless of its garbage value byte, and a row that is `FALSE` (`value = 0`) is rejected regardless of validity. Only `(valid, TRUE)` survives. (Because the value side is byte-per-row and validity is bit-per-row, this is a per-row `valid(i) && value(i)` test, not a single word-parallel AND of two identically-packed bitmasks — the two sides have different layouts.)

**Walk-through — why a NULL predicate row is dropped:** for `WHERE x > 5` with `x IS NULL`, the predicate result for that row is `NULL` (from the strict-comparison rule above): its validity bit is `0`. `Filter` requires `valid(i) && value(i)`; `valid(i)` is `0`, so the row is **not selected**. This matches SQL: `WHERE unknown` does not retain the row.

**The negation/totality surprises.** `WHERE NOT (x > 5)` does *not* recover that row — `NOT NULL = NULL`, still rejected. Nor does `WHERE x > 5 OR x <= 5`: with `x IS NULL` both disjuncts are `NULL`, and `NULL OR NULL = NULL` (the OR table below), so the row drops even though the predicate "looks" like a tautology. `NULL` is not the negation-complement of anything; that is the canonical 3VL gotcha, and our Filter rule handles it for free precisely because it tests `valid && value`.

### Logical connectives: 3VL, not mask-AND

The subtle, must-get-right part. The Boolean connectives are **not** strict and **cannot** be implemented as a bitwise AND/OR of value bits plus a bitwise AND of validity bits. `AND` short-circuits on `FALSE` *across the NULL boundary*: `FALSE AND NULL` is `FALSE`, not `NULL`, because a single `FALSE` operand makes the result `FALSE` no matter what the other operand turns out to be. Symmetrically for `OR` and `TRUE`.

Full truth tables (`T` = true, `F` = false, `N` = NULL/unknown):

**AND**

| AND | T | F | N |
|-----|---|---|---|
| **T** | T | F | N |
| **F** | F | F | F |
| **N** | N | F | N |

**OR**

| OR  | T | F | N |
|-----|---|---|---|
| **T** | T | T | T |
| **F** | T | F | N |
| **N** | T | N | N |

**NOT**

| NOT |   |
|-----|---|
| **T** | F |
| **F** | T |
| **N** | N |

The governing rules:

- **AND** = `FALSE` if **either** operand is `FALSE` (even if the other is `NULL`); else `NULL` if **either** operand is `NULL`; else `TRUE`.
- **OR** = `TRUE` if **either** operand is `TRUE` (even if the other is `NULL`); else `NULL` if **either** operand is `NULL`; else `FALSE`.
- **NOT** flips `TRUE`/`FALSE` and leaves `NULL` as `NULL` (validity passes through unchanged; value bit flipped).

These are implemented as **NULL-aware scalar loops**, evaluating `(value, validity)` per row against the rules above.

**Why scalar — the honest reason.** It is *not* that 3VL is intrinsically un-vectorizable. AND/OR over two `(value, validity)` lanes are pure **branch-free, word-parallel bit algebra** — no per-row branch, no cross-lane shuffle — and production vectorized engines (DuckDB included) do exactly that. For `AND` the result value is `value_a & value_b`, and the result validity is `(valid_a & valid_b) | (valid_a & ~value_a) | (valid_b & ~value_b)` (valid when both inputs are valid, *or* when either input is a decisive `FALSE`); `OR` is the dual. All ANDs/ORs/NOTs, no branches. We keep the connectives **scalar anyway** for a pragmatic reason: they are rarely the bottleneck relative to the strict math, and the bitmask formulation is not worth the kernel complexity at this stage. This is a *cost/benefit* decision, not a claim of impossibility — and it is consistent with the SIMD-ceiling guidance in `LIMITATIONS.md` §3 (SIMD earns its keep on dense numeric loops; "branchy / rarely-hot glue" is where the payoff thins, here because we *choose* not to write the kernel, not because one cannot exist).

### Selection-vector discipline: Filter selects, Project materializes

`Filter` does **not** physically delete rows or compact columns. It produces (or refines, when chained) a **selection vector** — the list of surviving row indices — and passes the otherwise-unchanged `DataChunk` downstream. Materialization is deferred to `Project`, which evaluates its output expressions and **gathers** only the selected rows into a compact output chunk.

What this saves, stated precisely (no overclaim):

- **Predicate evaluation is unavoidably all-rows.** To know which rows survive, `Filter` must evaluate the predicate over **every input row** of the predicate column(s). That scan is not "live rows only" and is not what the selection vector saves.
- **The saving is on *materialization*.** A physical filter would copy *every* column down to the surviving rows at the `Filter` site. For a 30-column table where the `WHERE` touches 1 column, that is 29 columns of copying the filter has no reason to do. With a selection vector, `Filter` writes only a small index array; the non-predicate, projected columns are **never copied at the filter** and are touched exactly once, at the **gather in `Project`**, for live rows and projected columns only.
- So materialization cost goes from "copy all columns × all rows at the filter" to "gather live, *projected* columns × live rows, once" — while predicate evaluation stays "all rows × predicate columns." Unreferenced columns are free.

The tradeoff is honest, and stated next to the win: the selection vector adds a level of indirection — the `Project` gather is random-access / gather-shaped rather than sequential — and a very low-selectivity filter (almost everything passes) does that indirection for little compaction benefit. We accept this; analytical `WHERE`s are usually selective, and avoiding wide-column copies dominates.

## Alternatives Considered

- **Tree-walking interpreter with per-row dispatch.** Evaluate the expression tree one row at a time. Rejected: it reintroduces exactly the per-tuple interpretation overhead (per-node function-call/dispatch and operand setup) that the vectorized model exists to amortize (Boncz, Zukowski, Nes, *MonetDB/X100: Hyper-Pipelining Query Execution*, CIDR 2005). Our model dispatches once per batch per node instead of once per row per node.

- **Just-in-time compiling the expression tree to native code.** Generate machine code per query, fusing the whole tree into one loop (the data-centric/compiled approach). Rejected for now as scope and dependency cost; the vectorized interpreter is simpler, debuggable, and competitive — the vectorized-vs-compiled tradeoff is itself nuanced and workload-dependent (Kersten, Leis, Kemper, Neumann, Pavlo, Boncz, *Everything You Always Wanted to Know About Compiled and Vectorized Queries But Were Afraid to Ask*, PVLDB 11(13), 2018). A possible future direction, not a current need.

- **Implementing logical AND/OR as bitwise value-AND/OR with validity-AND.** Tempting because strict functions *are* exactly that. Rejected because it is **wrong**: it would compute `FALSE AND NULL = NULL` instead of `FALSE`, and `TRUE OR NULL = NULL` instead of `TRUE`. 3VL connectives are not strict; their result validity depends on operand *values* (a decisive `FALSE`/`TRUE` forces a valid result even against a NULL partner). The correct branch-free formulation is the value+validity bit algebra given above; we currently realize it as scalar 3VL loops.

- **A SIMD kernel for the logical connectives.** Rejected **on cost/benefit, not feasibility.** The 3VL connectives *are* expressible as a handful of branch-free word-parallel bit ops over value+validity masks (no branches, no cross-lane shuffles), so they could be vectorized. We do not, because they are rarely the bottleneck relative to the strict math and the bitmask kernel is not worth the complexity now. (This is the same reason recorded in the connectives section — the two statements are deliberately consistent: scalar is a pragmatic choice, never a claim that 3VL cannot be vectorized.)

- **Physically filtering (compacting all columns) at the Filter operator.** Rejected: copies unreferenced columns the predicate never reads. The selection-vector approach defers and minimizes *materialization* (predicate evaluation is still all-rows; see above).

## Consequences

**Wins**

- **Correct 3VL**, matched against DuckDB on NULL-containing rows — the cases that silently break naive implementations.
- **Amortized interpretation overhead**: one Highway dispatch per node per batch for numeric work (target resolved once and the pointer cached); no per-row dispatch in the hot numeric path.
- **NULL propagation is nearly free**: strict-function NULL handling is one bitmask `AND` per batch, skipped entirely on the all-valid fast path (ADR 0003), and leaves the SIMD inner loop branchless.
- **Cheap, deferred materialization**: `Filter` writes only an index array; wide unreferenced columns are never copied; live, projected rows are gathered once at `Project`. (Predicate evaluation itself is still all-rows over the predicate columns — the win is on materialization.)
- **Clean layering**: numeric kernels stay leaf and branch-free; the executor composes them; operators own selection/materialization.

**Tradeoffs (as-built simplifications, deliberate — not bugs)**

1. **Constants are broadcast to flat vectors.** Both kernel operands are uniform flat arrays, so the kernels need only one code path. This wastes lanes and memory. ADR 0005 *defines* a `constant` vector kind and motivates it via scalar broadcast (compare/add against a single scalar without 2048 copies of the literal); the P3 executor **does not yet route through it** — it materializes the broadcast, i.e. the executor currently does the thing ADR 0005 listed as its rejected "materialize 2048 copies" alternative. So that ADR 0005 win is **pending, not realized**; a constant-vector fast path is a named deferred optimization.
2. **One intermediate result `Vector` allocated per node.** No expression-result pooling and no in-place evaluation yet — a tall tree allocates a result buffer per node per chunk. A buffer pool / in-place rewrite for unary chains is deferred.
3. **String comparison and the logical connectives run in scalar.** Strings blunt SIMD (ADR 0004); the 3VL connectives *could* be vectorized as branch-free bit algebra but are deliberately left scalar as not-worth-it-yet (above). Both are intentionally off the SIMD path.
4. **Integer arithmetic wraps** (two's-complement), via Highway's defined-wrapping vector ops — **UB-clean under the `asan-ubsan` preset** (ADR 0002), not a naive signed `+` that UBSan would flag. Documented, not silently surprising.

All four are scoped, named, and measurable — points on the optimization roadmap, with the correctness contract already met.

## How to defend this at a whiteboard

- **The third truth value is the whole game.** SQL is 3VL: `TRUE`/`FALSE`/`NULL`. `x > 5` on a NULL `x` is `NULL` (unknown), not `FALSE`. `WHERE` keeps only `TRUE`; `FALSE` and `NULL` are both dropped. `WHERE x > 5 OR x <= 5` still drops a NULL `x` — `NULL OR NULL = NULL` — because NULL is not anyone's complement.
- **Strict functions: compute-then-mask.** Evaluate the value for every row (garbage in NULL lanes is fine), then set result validity = AND of operand validity masks. With ADR 0003's `1 = valid`, NULL-propagation *is* bitwise AND, and the all-valid case is an O(1) skip. For `x > 5`, the literal is all-valid, so result validity is just `validity(x)` — the AND only does work when both operands are nullable. Branchless inner loop is the point.
- **How a comparison becomes a stored bit.** Compare in the operand's type domain; narrow the mask to a `uint8`-per-row result via `DemoteTo(Rebind<uint8_t>, vec)`. For `double`, route the `f64` mask into the signed-int domain with `RebindMask` first, then `DemoteTo`. Boolean result vector = `uint8` value bytes + validity bitmask.
- **Logical AND/OR are NOT mask-AND — this is the trap.** `FALSE AND NULL = FALSE`, `TRUE OR NULL = TRUE`; `NULL` only survives when no operand is decisive. The result *validity* depends on operand *values*, so it is not a pure validity-mask op. I can write the three truth tables from memory.
- **Why logical ops are scalar — say it precisely.** Not because 3VL is un-vectorizable: AND/OR are branch-free word-parallel bit algebra over value+validity (`valid_out = (valid_a & valid_b) | (valid_a & ~value_a) | (valid_b & ~value_b)` for AND), exactly how DuckDB does it. We keep them scalar because they are rarely the bottleneck and the kernel is not worth it yet — a cost/benefit call, not a feasibility claim.
- **NEON lane reality, so I don't overclaim arithmetic.** 128-bit NEON = 4× `int32` but only 2× `int64`/`double`; AVX2 doubles both. TPC-H math is `int64`/decimal/`double`, i.e. the 2-lane types on the dev machine — so arithmetic speedup is width-dependent and measured per type in `BENCHMARKS.md`, not asserted as uniformly cheap.
- **Wrapping arithmetic is UB-clean.** Two's-complement wrap via Highway's defined-wrapping vector ops, not a source-level signed `+` (which would be UB and trip the `asan-ubsan` preset, ADR 0002).
- **Selection vector vs physical filter.** `Filter` emits row indices, not compacted columns. Predicate evaluation is still all-rows over the predicate column — unavoidable. The saving is on *materialization*: non-predicate projected columns are copied once, at the `Project` gather, for live rows only, instead of being copied at the filter. The gather is random-access; for selective filters that wins easily.
- **One dispatch per batch per node.** Post-order tree walk; each numeric node makes a single Highway dispatch over up to 2048 rows, amortizing per-tuple interpretation overhead (Highway resolves and caches the SIMD target once). That is the X100 vectorized thesis (Boncz, Zukowski, Nes, CIDR 2005).
- **What I knowingly left on the table.** Constants are broadcast (ADR 0005's constant-vector win is pending, not realized), a result `Vector` is allocated per node (no pooling/in-place yet), strings/logic are scalar, integer math wraps. Each is a named, measurable optimization, not a correctness gap — the DuckDB diff already passes.
