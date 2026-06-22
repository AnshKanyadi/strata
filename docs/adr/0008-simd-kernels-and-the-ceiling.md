# 0008. SIMD kernels: per-batch dispatch boundary, Highway runtime dispatch, and the SIMD ceiling

## Status

Accepted — implemented as-built in P3.

> **Measured outcome (NEON, M3 Pro — see [BENCHMARKS.md](../BENCHMARKS.md)).** Two
> places where the data refined the predictions below:
> 1. **Comparison did *not* win meaningfully less than arithmetic.** int32
>    comparison (~3.8×) ≈ int32 arithmetic (~3.6–3.7×); the predicted
>    select+demote+byte-store penalty turned out small on NEON. The dominant
>    factor was **lane count**, not the comparison result path: 32-bit kernels
>    win ~3.7× (4 lanes) while doubles win only ~1.7× (2 lanes). Read the "wins
>    less" reasoning below as *the mechanism that exists*, whose magnitude the
>    benchmark showed to be minor relative to lane width.
> 2. **The comparison byte store is, as-built, `StoreU` on a narrow `Rebind<uint8_t>`
>    tag** (which writes exactly the lane-count bytes — a partial store), not the
>    `StoreN(...)` spelling shown in the snippets below. Both are equivalent
>    partial stores; the differential test validates the result across all
>    non-lane-multiple sizes.

## Context

Strata is a vectorized, push-based engine: operators process columnar `DataChunk`s of up to ~2048 rows (ADR 0007). The numeric core of execution — projecting `a + b`, filtering `c < k` — must run as data-parallel SIMD over flat, aligned column buffers (ADR 0005), not as scalar per-row loops. That is the entire thesis of the X100/vectorized design (Boncz, Zukowski, Nes, CIDR 2005): amortize interpretation overhead across a batch and let the loop body be a tight, branch-free, vectorizable kernel.

Two constraints shape the implementation:

1. **One source, two ISAs.** The development machine is an Apple M3 Pro (ARM NEON, 128-bit, no AVX); x86_64 CI has at least AVX2 (256-bit), recorded per run. We want a single kernel source that emits good code on both, with the *actual* CPU target chosen at runtime — not a compile-time `-march` guess baked into one binary. The P0 preflight (`strata --version`) already prints the dispatched SIMD target (e.g. `NEON_BF16` on the M3), so this machinery is observable.

2. **No SIMD types in the public surface.** Operators (ADR 0006) must not include or name vector types. The kernel layer has to present a plain C++ boundary and keep all `hwy::` types behind it.

The honest second half of this ADR is the *ceiling*: SIMD is not a uniform multiplier. It pays off enormously for some operations and barely at all for others, and an interview-grade systems credential states *where* and *why* without hand-waving. The measured numbers live in `docs/BENCHMARKS.md`; this ADR explains the mechanism and frames the *expected shape* of the results, and `docs/LIMITATIONS.md` records the consequences.

## Decision

Implement numeric kernels with **Google Highway (1.4.0) using runtime dispatch.** The kernel source is compiled once per SIMD target via `foreach_target.h`; `HWY_EXPORT` registers the per-target functions, and `HWY_DYNAMIC_DISPATCH` selects the CPU-detected target at runtime. The *same* source therefore emits NEON on the M3 and AVX2 on the x86 CI.

**Kernel set.** Binary arithmetic `{+, -, *}` and comparison `{=, !=, <, <=, >, >=}` for `int32`, `int64`, and `double`.

**Public surface.** Two plain C++ overloaded entry points — `Arith(...)` and `Compare(...)` — taking flat value pointers, a count, and an op enum. They expose **no** Highway types. Everything `hwy::` lives in the `.cc` behind these overloads.

### Dispatch once per batch, never per value

The public entry point performs **exactly one** `HWY_DYNAMIC_DISPATCH` per call — i.e. once per batch of ~2048 values. `HWY_DYNAMIC_DISPATCH` is an indirect call through a function pointer; paying it per value would reintroduce, per element, precisely the interpretation overhead the vectorized model exists to amortize. Inside the dispatched function, a **single `switch` on the op enum** selects a branch-free inner loop; the op decision is hoisted out of the loop, so the loop body has no per-value branching and no per-value indirect call. This boundary — dispatch and op-dispatch hoisted above the hot loop — is the load-bearing design choice of the whole kernel layer.

### Value/validity separation

Kernels operate on **flat arrays of values only.** They know nothing about NULLs. Three-valued NULL handling happens *outside* the kernel by combining validity bitmasks (ADR 0003): a handful of `uint64`-wide ANDs, or skipped entirely on the all-valid fast path (unallocated mask == all-valid, O(1)). Keeping NULL logic out of the kernel is exactly what keeps the inner loop branch-free and lane-friendly — a per-value "is this null?" test would be data-dependent control flow and would defeat vectorization.

### Comparison output is narrowed bytes

Comparisons emit a `uint8` `0/1` per row, not a packed bitmask. Predicate *results* are bytes so downstream selection-vector construction has addressable per-row values; validity, by contrast, is a packed `uint64` bitmask (ADR 0003). That asymmetry is deliberate: converting predicate bytes to a packed bitmask for predicate combination (`AND`/`OR` of two predicate results) is a deferred option, flagged for `BENCHMARKS.md` if predicate-combination store bandwidth shows up.

The narrowing from a wide lane mask to a byte is explicit, and the demoted result occupies only the low lanes, so it is stored with a partial (low-`N`) store rather than a full-register store:

- For `int32`/`int64`: materialize the compare mask into a 0/1 vector in the **wide** value domain — `auto ones = IfThenElseZero(mask, Set(d_wide, 1));` — then demote to bytes through a `Rebind` tag that keeps the lane count, and store only the low `N` bytes: `StoreN(DemoteTo(Rebind<uint8_t, decltype(d_wide)>(), ones), d_u8, out, n);`. `DemoteTo` via a `Rebind` tag yields the `uint8` results in the low lanes only, which is why the store is partial.
- For `double`: there is no direct `f64 -> uint8` narrowing, and the compare mask is bound to the `f64` lane width. `RebindMask` the mask onto the same-lane-count signed-integer (`i64`) descriptor, **materialize it to a 0/1 `i64` vector** (`IfThenElseZero` over the `i64` descriptor — you cannot `DemoteTo` a mask), then `DemoteTo` that vector to `uint8` and partial-store.

### Integer arithmetic wraps (two's complement)

Integer `+ - *` **wrap** on overflow. The scalar reference computes via unsigned casts so signed-overflow UB never occurs — it is **UBSan-clean**. Highway's int add/sub/mul lower to hardware instructions and are defined by Highway as two's-complement wrapping ops, independent of C++ signed-overflow UB. Both therefore produce the same low-width wrapped result, so the SIMD path and the scalar reference **agree by construction** (for multiply, on the low half of the product — both discard the high half identically). This is a deliberate, documented **simplification**: production engines (DuckDB) instead raise an overflow error or promote the result type. Aggregate `SUM` widening is a separate concern handled in P4. We do not claim DuckDB-equivalent overflow behavior here.

### Scalar tail

Counts that are not a multiple of the lane width leave a remainder. Highway provides masked / partial-vector stores (`StoreN`, `MaskedStore`, the `Transform` helpers) that can subsume the remainder in a masked final iteration; we use a plain scalar tail loop instead for clarity, since at ~2048-row batches the remainder is fully amortized. It is real code and a real (small) cost, but the choice is simplicity, not necessity.

### Differential test oracle

Correctness is pinned by a **hand-written, independent scalar reference** — deliberately *not* Highway's `HWY_SCALAR` target, so a bug in Highway (or in our use of it) cannot be masked by testing Highway against itself. The differential test generates random inputs including NULLs, runs both the dispatched SIMD kernel and the scalar reference, and asserts the outputs are identical (values where valid; validity bitmask composition included).

## Alternatives Considered

- **Compile-time `-march` / single static target.** Bakes one ISA into the binary; either CI and dev diverge, or we ship a lowest-common-denominator build. Rejected: we explicitly want the *same source* validated on NEON and AVX2, with the target chosen by the CPU at runtime. Runtime dispatch is the feature, not overhead to avoid.

- **Hand-written NEON + hand-written AVX2 intrinsics.** Two code paths to write, test, and keep in sync; no portability beyond the two we hand-code; far more surface for a divergence bug. Highway gives one portable source with runtime dispatch for the cost of learning its idioms. Rejected on maintenance and correctness grounds.

- **Autovectorization (let the compiler do it).** Fragile and non-portable in practice: a small change to the loop, aliasing assumptions, or the NULL handling can silently drop the loop back to scalar with no diagnostic. Highway makes the vectorization explicit and target-visible (and the preflight prints the target). Rejected: too easy to regress invisibly.

- **`HWY_DYNAMIC_DISPATCH` per value / op-branch inside the loop.** This is the anti-pattern the per-batch boundary exists to prevent. An indirect call or an op `switch` per element reintroduces exactly the per-row overhead vectorization is meant to kill. Rejected categorically.

- **Comparison output as a packed 1-bit-per-row bitmask.** Saves store bandwidth, but the consumer (selection vectors, downstream predicate combination) wants addressable per-row results, and producing a packed mask portably across ISAs adds its own complication. We chose `uint8 0/1` as the simpler, portable contract and accept the demote/store cost. (Revisit if filter or predicate-combination store bandwidth shows up in `BENCHMARKS.md`.)

- **Exposing Highway types in the public kernel API.** Would leak `hwy::` into operator code (ADR 0006) and couple the executor to a SIMD library's types. Rejected: the plain `Arith`/`Compare` overloads are the abstraction boundary.

## Consequences

**Wins**

- One kernel source runs as NEON on the M3 and AVX2 on x86 CI, target chosen at runtime and printed by the preflight; no per-ISA fork.
- The hot loop is branch-free: op dispatch and CPU-target dispatch are both hoisted above the ~2048-element loop, paid once per batch.
- NULL handling composes cheaply outside the kernel as `uint64` bitmask ANDs, with an O(1) all-valid fast path (ADR 0003); the kernel stays a pure value loop.
- A constant operand is loaded once and broadcast into a SIMD register (`Set`/`Broadcast`) per ADR 0005, so the kernel runs at full lane utilization with no per-row materialization of the constant.
- Correctness is anchored by an independent scalar oracle, not by Highway-against-Highway.
- Integer arithmetic is UBSan-clean in the reference and matches Highway's wrapping SIMD semantics exactly.

**Tradeoffs (stated honestly)**

- **Comparison result path.** Comparison does strictly more work than same-width arithmetic and its result path runs narrower (see the ceiling below): a select to 0/1, a demote to bytes, and a byte-width partial store.
- **Scalar tail.** A scalar remainder loop (rather than a masked final iteration) is a deliberate simplicity choice; it is real, small code.
- **Wrapping integer semantics.** We wrap where DuckDB would error or promote — a deliberate divergence from a production engine, documented here and in `LIMITATIONS.md`.
- **Constant-operand code path.** Broadcasting a constant via `Set` is full-utilization (not a wasted-lane cost), but it does mean a separate scalar-broadcast kernel variant / extra code path alongside the column-column path. See the companion P3 ADR on the executor/constant handling.

## The SIMD ceiling

This is the centerpiece. SIMD is not a flat multiplier; it pays off in proportion to lane utilization and the absence of data-dependent control flow. The bullets below are **mechanism-driven expectations to be confirmed by `docs/BENCHMARKS.md`**, not measured results, and no speedup numbers are quoted here.

- **Mechanistically the best case — branch-free, same-width arithmetic over contiguous flat columns.** Pure load-load-op-store at the same width with full lane utilization, no select, no narrowing, no branches. This is where the architecture is expected to earn its keep.

- **Expected to win less — numeric comparison / filter.** Arithmetic is same-width load-op-store at full lane utilization. Comparison additionally turns a lane mask into 0/1 (a select), then demotes the wide lanes to bytes and stores at byte width — extra instructions and a result path that no longer runs at the input lane width. That added select + demote + byte-store chain, *not narrowing alone*, is why comparison is expected to win less than arithmetic.

- **Width effect (orthogonal to op type).** Wider element types halve the parallelism on a 128-bit register: NEON fits 4 `int32` but only 2 `int64`/`double` per 128-bit register. This applies **equally to arithmetic and comparison** — 64-bit work of any kind is roughly half the throughput of 32-bit on the same hardware. It is a width effect, not a comparison-vs-arithmetic effect.

- **Expected little or no win — strings, hash-table probes.**
  - *Strings* (ADR 0004) are 16-byte German strings: equality short-circuits on a 4-byte length and a 4-byte inline prefix, reaching `memcmp` of the variable-length tail only when both match. The metadata words (length/prefix) sit at a fixed 16-byte stride, so they can be loaded across lanes for a bounded reject-phase win — a strided/deinterleave load, not a flat one, and NEON has no native gather. The variable-length tail is a byte loop / `memcmp` that does not vectorize per-element. Net: little SIMD win on string-heavy operators, but more than the naive "it's all `memcmp`" framing implies.
  - *Hash-table probes* are random gather / pointer-chasing; the bottleneck is memory latency, not arithmetic throughput, so lanes sit idle waiting on loads.

- **Three-valued logical ops deferred to the P3 TVL ADR.** Boolean connectives (`AND`/`OR`/`NOT`) over nullable booleans are a separate concern, decided in the companion P3 three-valued-logic ADR, not here. We do **not** claim TVL is fundamentally un-vectorizable: Kleene logic composes from a handful of bitwise ops over (value-bitmask, validity-bitmask) pairs — exactly the bit-parallel style as the validity AND in ADR 0003 — and that bit-parallel evaluation is the vectorizable alternative on the table. The implementation choice (bit-parallel vs scalar, weighed for P3 scope/simplicity) belongs to that ADR.

- **NEON ceiling vs paper figures.** NEON is 128-bit with no AVX, so M3 speedups are mechanistically expected to be more modest than the AVX2/AVX-512 figures common in the literature. We therefore report **both** NEON (Mac) and AVX2 (x86 CI) numbers in `BENCHMARKS.md` rather than quoting one and implying the other.

The takeaway: the speedups in `BENCHMARKS.md` are *expected* to be uneven across operators, and that unevenness is a property of the hardware and the data layout, not a bug or a tuning failure.

## How to defend this at a whiteboard

- "Why Highway instead of intrinsics?" One portable source, runtime-dispatched to the actual CPU; we validate the *same* code on NEON (M3) and AVX2 (CI) instead of maintaining two hand-written paths that can silently diverge.
- "Where's the dispatch?" Once per batch. `HWY_DYNAMIC_DISPATCH` is an indirect call; the op `switch` is hoisted above the loop. Per-value dispatch would reintroduce exactly the interpretation overhead vectorization exists to amortize.
- "How do NULLs work in the kernel?" They don't — kernels are pure value loops. NULLs are `uint64` bitmask ANDs done outside, with an O(1) all-valid fast path (ADR 0003). That separation is what keeps the loop branch-free.
- "Why is comparison expected to be slower than add?" Add is same-width load-op-store. Comparison adds a select to 0/1, a demote of wide lanes to bytes, and a byte-width partial store — the select + demote + store chain, not narrowing alone. For `double`, the compare mask routes onto the `i64` descriptor via `RebindMask`, materializes to a 0/1 `i64` vector, then `DemoteTo` `uint8`.
- "Why do 64-bit ops run slower?" Width, not op type: 128-bit NEON fits 4 `int32` but only 2 `int64`/`double`, so 64-bit arithmetic *and* comparison get half the lanes.
- "What about overflow?" The scalar reference uses unsigned casts (UBSan-clean); Highway's int ops are hardware-mapped two's-complement wrapping. They agree on the low-width wrapped result by construction (for mul, on the low half). It's a documented simplification; DuckDB errors or promotes, and SUM widening is handled in P4.
- "How do you know the SIMD is correct?" Differential test against an *independent* hand-written scalar reference (not Highway's scalar target), on random inputs including NULLs.
- "What about a constant operand?" Loaded once and broadcast into a register (`Set`), full lane utilization, no 2048-copy buffer (ADR 0005).
- "Why not SIMD everything?" The ceiling: best for flat same-width arithmetic; less for comparison (select + demote + byte-store) and for 64-bit types (half the lanes on 128-bit NEON); little for strings (length/prefix short-circuit then `memcmp` tail, only the strided metadata loads across lanes) and hash probes (gather/latency-bound). Three-valued logic is deferred to the P3 ADR — bit-parallel Kleene logic is vectorizable, so we don't claim otherwise.
- "Why are the M3 numbers expected to be smaller than the papers?" 128-bit NEON, no AVX. We report both NEON and AVX2 so the comparison is honest.
