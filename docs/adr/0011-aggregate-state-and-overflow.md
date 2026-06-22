# 0011. Aggregate state, NULL semantics, and the SUM overflow policy

## Status

Accepted.

## Context

P4 adds hash aggregation (ADR 0010 owns the group-key hash table and the rule that all NULL keys collapse to one group). This ADR documents the *aggregate computation* layer that sits on top of that table: which aggregates exist, how their running state is laid out and updated, how three-valued NULL logic (ADR 0009) flows through them, and — most importantly — what happens at the numeric boundaries.

The engine supports five aggregate forms over numeric types:

| Aggregate    | Input types                          | Output                       |
|--------------|--------------------------------------|------------------------------|
| `COUNT(*)`   | none (counts rows)                   | `int64`                      |
| `COUNT(col)` | any                                  | `int64`                      |
| `SUM`        | `int32`, `int64`, `double`           | `int64` (ints), `double`     |
| `MIN`/`MAX`  | `int32`, `int64`, `double`, `date`   | same as input                |
| `AVG`        | `int32`, `int64`, `double`           | `double`                     |

Two cross-cutting concerns force explicit decisions:

1. **NULL semantics.** SQL aggregates are *not* uniform about NULL: most skip NULLs, `COUNT(*)` does not, and the "no rows survived" case must return NULL for some aggregates and 0 for others. Getting this wrong silently corrupts results against the DuckDB oracle.
2. **Overflow and rounding.** A `SUM` over a large column is the single place an analytical engine is most likely to produce a *numerically wrong* answer that still looks plausible. We must state our accumulator widths and our floating-point summation guarantees honestly, because we are validated against DuckDB and DuckDB makes a *different* choice (HUGEINT) that we deliberately do not match.

## Decision

### State structs, stored inline in the group row

Each aggregate keeps a fixed-size state struct, stored inline in the group's row buffer in the hash table (ADR 0010). No per-group heap allocation; the state lives where the group key lives, so an update is a write through the group's row pointer.

```cpp
struct CountState  { int64_t count;                  };  // COUNT(*) and COUNT(col)
struct SumState    { Accum   sum;   bool has_value;  };  // Accum = int64_t or double
struct MinMaxState { T       value; bool has_value;  };  // T matches input type
struct AvgState    { Accum   sum;   int64_t count;   };  // Accum = int64_t or double
```

`Accum` is the *widened* accumulator type, resolved per input type at setup (see overflow policy). `has_value` (for SUM/MIN/MAX) and `count == 0` (for AVG) are how we distinguish "the SQL result is NULL" from "the result is the additive identity 0".

### Per-batch update functions, not per-row dispatch

For every `(aggregate, input-type)` pair we resolve **once at operator setup** to a concrete, monomorphized update function. That function takes the input vector, the precomputed group-index array (row *i* belongs to group `group_index[i]`, produced by the probe in ADR 0010), and the row count, and **loops internally** over the whole batch.

There is **no per-row virtual call and no per-row function pointer.** The type switch and the aggregate switch are paid once per chunk (at most once per 2048 rows), then the inner loop is a tight, inlinable, branch-predictable scalar loop over a typed accumulator indexed by group. The body specializes on whether the input vector carries a validity mask (the all-valid O(1) fast path of ADR 0003 skips the NULL check entirely).

**Why the inner loop is scalar (and *not* SIMD like ADR 0008).** The update is a *scatter*: `state[group_index[i]] op= value[i]`. Consecutive rows can target the same or arbitrary group slots, so it is a read-modify-write with potential same-lane conflicts (two rows landing in one group within a single SIMD register would clobber each other). That is not amenable to straight SIMD lanes the way the *elementwise* ADR 0008 kernels are. So this ADR shares only the **dispatch discipline** of ADR 0008 — resolve type/op once per batch, then run a tight monomorphized inner loop — **not** its execution model. The win here is dispatch-once-per-batch plus tight inlining and branch prediction, **not** vectorization. SIMD speedups in the ADR 0008 sense do not apply to scatter aggregation.

### State lifecycle

1. **Init** — when a new group row is created in the hash table, its state structs are zero/identity-initialized: `count = 0`, `has_value = false`, `sum = 0`.
2. **Update** — once per input batch, the resolved per-batch function folds the batch into the per-group states via `group_index`.
3. **Finalize** — once, after the build side is fully consumed, each state is converted to an output cell (with NULL semantics applied, below). Finalize is also where AVG performs its single division and where integer-SUM/AVG widening is reflected in the output type.

### NULL semantics (three-valued, explicit)

- **Aggregates skip NULL inputs**, with one exception: `COUNT(*)` counts **every** row regardless of any column's NULL-ness (it has no argument column).
- `COUNT(col)` counts only rows where `col` is non-NULL.
- `SUM`/`MIN`/`MAX`/`AVG` ignore NULL inputs: a NULL input row leaves the state untouched (no `has_value` flip, no `count` bump).
- **Empty group or all-NULL aggregated column:** `SUM`, `MIN`, `MAX`, `AVG` finalize to **NULL** (detected by `has_value == false`, or `count == 0` for AVG). `COUNT(*)` and `COUNT(col)` finalize to **0, never NULL.**
- **NULL group keys** are out of scope here — ADR 0010's hash table maps all NULL keys to a single group; aggregation just sees that group's `group_index` like any other.

### MIN/MAX seeding

MIN/MAX have no additive identity to pre-seed (and seeding with `INT_MAX`/`INT_MIN` would be wrong for `date` and brittle in general). Instead the **first non-NULL value seeds** the state: if `!has_value`, store the value and set `has_value = true`; otherwise compare and conditionally replace. A group that never sees a non-NULL value keeps `has_value == false` and finalizes to NULL, which is exactly the all-NULL rule above.

**NaN for `double` MIN/MAX is a known potential divergence and is currently out of scope.** The seed-on-first-non-NULL logic uses plain IEEE `<` / `>`, and every comparison involving NaN is false, so whether a NaN ends up retained depends on arrival order, and "skip NULL" does not skip NaN (NaN is a value, not NULL). DuckDB orders NaN as greater than all other doubles for MIN/MAX. We do **not** currently replicate that ordering and do not test for it; this is flagged as a concrete, accepted divergence risk rather than silently assumed correct, and would need explicit NaN handling if a target query exercised it.

### SUM overflow policy (the honesty centerpiece)

- **`SUM(int32)` accumulates into `int64`.** Widening the accumulator is the canonical reason analytical engines widen: an `int64` cannot overflow from summing `int32` values for any realistic row count. The headroom is `INT64_MAX / INT32_MAX ≈ 2^32` — you would need on the order of **2^32 rows (roughly 4 billion) of near-`INT32_MAX` values** to even approach the `int64` ceiling. A regression test sums `int32` values whose *true* total exceeds `INT32_MAX` and asserts the `int64` result is exact — a naive `int32` accumulator would silently wrap and this test would catch it.

- **`SUM(int64)` accumulates into `int64` and outputs `int64` — deliberately NOT promoted to `int128`.** Stated honestly: **DuckDB sums SMALLINT/INTEGER/BIGINT into `HUGEINT` (`int128`)**, giving it a vastly larger range before overflow than our `int64` (and DuckDB *raises* an overflow error rather than wrapping). So DuckDB will not overflow in cases where we can — though note `int128` is still finite and DuckDB itself can overflow on sums whose true total exceeds `2^127`. We keep `int64` because `__int128` is a **non-standard GNU extension**: GCC emits *"ISO C++ does not support `__int128` for `type name` [-Wpedantic]"* in type-name/cast contexts, and our CI matrix (ADR 0002) holds *all* first-party engine code to `-Wpedantic -Wextra -Werror` on **both** Homebrew clang (mac/NEON) **and** gcc-14 (Ubuntu/AVX2). Narrow escape hatches do exist (the library spellings `__int128_t`/`__uint128_t`, or isolating the accumulator behind a typedef in a system-header-marked wrapper, or a localized `#pragma` suppression) — i.e. matching DuckDB does *not* strictly require dropping `-Wpedantic` project-wide. We judged the added per-element `int128` arithmetic cost and the wrapper complexity not worth it for the current TPC-H scope, and we will not pepper the whole accumulator with portability pragmas. So we accept that **extreme `int64` sums can overflow.** This is a documented, deliberate scope/portability tradeoff, not an oversight; it is revisitable behind a portable `int128` wrapper if a specific target query (e.g. a TPC-H sum over a very large, very wide column) actually demands it.

- **`SUM(double)` accumulates in `double`.** Floating-point addition is **not associative**, so the result is **order-dependent** (rounding depends on the order rows are folded in, which depends on chunk boundaries and group layout). We do **not** implement compensated (Kahan/Neumaier) summation. DuckDB's own double SUM is *also* order-dependent (it uses SIMD partial-sum reduction trees), so exact bitwise agreement is not expected from either side. Our oracle harness therefore compares doubles with a **relative epsilon tolerance, not bitwise equality** (see the validation note below) — precisely because both engines do order-dependent, non-associative summation. This is stated here so the limitation is never a surprise.

### Validation tolerance for floating-point results

Because both engines sum non-associatively, the oracle comparison for `double` SUM/AVG (and any double-valued aggregate output) is **not bitwise**: results match if they agree within a small relative tolerance (a few ULPs / a relative epsilon), with an absolute-epsilon floor near zero. Integer and `date` aggregate outputs are compared **exactly**. This makes the floating-point validation claim falsifiable: a divergence beyond the relative tolerance is a real bug, agreement within it is expected. (If a tighter, named tolerance is fixed in the harness later, this ADR should be updated to cite the exact value; today the policy is "relative-epsilon, not bitwise.")

### AVG

`AVG` keeps the **same widened sum** as `SUM` plus an `int64 count`, and finalizes as `double(sum) / double(count)`, returning **NULL when `count == 0`**. **Integer AVG returns `double`** — the result *type* is `double`, with no truncation; it is not integer division. AVG inherits the SUM caveats and adds one of its own:

- **`int64`-sum overflow** on extreme integer inputs (same as SUM).
- **Order-dependent rounding** on `double` inputs (same as SUM).
- **Conversion precision loss for integer AVG:** we convert the `int64` sum to `double` before dividing, and a `double` has a 53-bit mantissa, so once `|sum|` exceeds `2^53` the conversion loses low-order bits *even when no overflow occurred*. DuckDB averages integers via its wider HUGEINT/decimal path and so can be more precise here. Integer AVG can therefore differ from DuckDB in the low bits **independently of overflow**; this is accepted under the same relative-epsilon validation tolerance as `double` SUM.

## Alternatives Considered

- **Per-row virtual dispatch / function-pointer-per-row for updates.** Rejected: a virtual call (or indirect call) per row defeats inlining and pays the type/aggregate decision millions of times instead of once per batch. Per-batch monomorphization is the whole point of a vectorized engine and is consistent with the *dispatch* discipline of ADR 0008.

- **SIMD the aggregate update loop (like ADR 0008 kernels).** Rejected as not applicable: the update is a per-group scatter (`state[group_index[i]] op= value[i]`) with read-modify-write and potential same-lane conflicts when multiple rows hit one group. Straight SIMD lanes do not express conflict-free scatter; the elementwise ADR 0008 kernels vectorize precisely because they have no such cross-lane dependency. The aggregate inner loop is therefore deliberately scalar.

- **`int128`/HUGEINT accumulator for integer SUM (DuckDB's choice).** Rejected *for now*: `__int128` is a GNU extension whose `-Wpedantic` diagnostic fires on our gcc CI leg. Matching DuckDB would mean either dropping `-Wpedantic` project-wide (weakening an invariant) or routing every accumulator through a pragma-guarded / system-header / `__int128_t`-spelled wrapper. The narrow workarounds exist but carry per-element `int128` arithmetic cost and complexity we judged unjustified for current TPC-H scope. We kept the strict-warnings invariant and documented the narrower numeric range instead. Revisitable behind a portable wrapper.

- **Kahan/compensated summation for `double` SUM.** Rejected: adds per-element work and state for a precision guarantee our relative-epsilon validation tolerance does not require — and DuckDB does not do it either, so it would not even buy closer oracle agreement. Documented as a known limitation rather than silently assumed.

- **Pre-seeding MIN/MAX with type sentinels (`INT_MAX`/`INT_MIN`/`±inf`).** Rejected: sentinels are wrong or fragile for `date` and `double` (NaN/inf interactions) and conflate "no value yet" with "saw the extreme value." The `has_value` seed-on-first-non-NULL approach is uniform across all supported types and falls straight out of the all-NULL → NULL rule.

- **Separate `COUNT(*)` and `COUNT(col)` code paths vs one `CountState`.** Unified on a single `CountState { int64 count }`; the only difference is whether the per-batch update consults the input validity mask (`COUNT(col)`) or counts unconditionally (`COUNT(*)`). One struct, two update bodies resolved at setup.

## Consequences

**Wins**

- Update is a tight typed loop over inline per-group state — no per-row dispatch, no per-group allocation, cache-friendly because state lives in the group row.
- NULL semantics are centralized and explicit: `has_value`/`count==0` cleanly separates "NULL result" from "zero result," matching SQL and the DuckDB oracle.
- `SUM(int32)` widening to `int64` removes the most common real-world overflow class outright (≈2^32-row headroom), with a regression test that fails loudly if the widening is ever lost.
- The lifecycle (init / per-batch update / single finalize) keeps the per-batch hot path free of finalization branches (e.g. AVG's division happens exactly once per group).

**Tradeoffs (stated, not hidden)**

- **`SUM(int64)`/`AVG(int64)` can overflow** where DuckDB's wider HUGEINT accumulator would not. Deliberate, for `-Wpedantic` portability across the clang+gcc matrix; revisitable behind a portable `int128` wrapper.
- **`double` SUM/AVG is order-dependent** (no Kahan); results may differ in the low bits depending on chunk/group ordering. Validated by relative-epsilon comparison, not bitwise.
- **Integer `AVG` loses low-order precision** for sums above `2^53` when the `int64` sum is converted to `double` before dividing — an oracle-divergence source independent of overflow.
- **`double` MIN/MAX NaN handling** does not currently match DuckDB's NaN-as-greatest ordering; a known potential divergence, scoped out until a query needs it.
- **MIN/MAX on VARCHAR is deferred** (and would be scalar if added): German-string comparison is branchy and out of the SIMD lane (ADR 0004/0008), so it is not part of the as-built numeric/date set.

## How to defend this at a whiteboard

- "Aggregate state is a fixed-size struct stored *inline in the group row* in the hash table — no per-group heap, the update writes through the group pointer."
- "Dispatch is resolved once per `(aggregate, input-type)` at setup, then a tight per-batch loop folds the batch via the group-index array — same *dispatch* discipline as the SIMD kernels (resolve type/op once per batch). No per-row virtual calls."
- "But the inner loop is *scalar*, not SIMD: the update is a scatter, `state[group_index[i]] op= value[i]` — read-modify-write with possible same-lane conflicts when two rows hit one group. That doesn't vectorize like the elementwise ADR 0008 kernels do. The win here is dispatch-once-plus-inlining, not vectorization."
- "NULL rule: everything skips NULLs *except* `COUNT(*)`, which counts every row. All-NULL or empty group: SUM/MIN/MAX/AVG return NULL via `has_value`/`count==0`; COUNT returns 0, never NULL."
- "MIN/MAX have no identity element, so I seed on the first non-NULL value with a `has_value` flag rather than a sentinel — that gives me the all-NULL→NULL case for free and works for `date`. NaN for double MIN/MAX I haven't matched to DuckDB's NaN-as-greatest ordering; I'll flag that as a known divergence, not a solved problem."
- "`SUM(int32)` widens to `int64` — that's the canonical fix. The headroom is `INT64_MAX / INT32_MAX ≈ 2^32`, so you'd need ~2^32 — about 4 billion — near-max rows to threaten the `int64` ceiling, and I have a regression test whose true sum exceeds `INT32_MAX`."
- "`SUM(int64)` I keep at `int64` and I'll tell you up front it can overflow. DuckDB sums into HUGEINT/`int128` — far larger range, and it raises on overflow rather than wrapping — so it won't overflow where I can (though int128 is still finite). I stay at `int64` because `__int128` is a GNU extension whose `-Wpedantic` diagnostic fires on my gcc CI leg. There are narrow workarounds — the `__int128_t` spelling, or a system-header-marked wrapper with a localized pragma — but I judged the per-element int128 cost and complexity not worth it for current TPC-H scope. Documented portability tradeoff, fixable behind a portable wrapper."
- "`double` SUM isn't associative — my result is order-dependent and I don't do Kahan. Neither does DuckDB, so exact bitwise match isn't expected from either side; my oracle harness compares doubles with a relative-epsilon tolerance, not bitwise equality."
- "Integer `AVG` returns `double`: I keep the widened sum and an `int64` count and divide once at finalize, returning NULL when count is zero. One extra caveat — I convert the `int64` sum to `double` before dividing, so above `2^53` the conversion drops low bits, which can diverge from DuckDB's wider integer-AVG path independently of overflow. Accepted under the same float tolerance."
