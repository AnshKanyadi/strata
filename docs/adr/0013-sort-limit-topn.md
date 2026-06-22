# 0013. Sort, Limit, and Top-N: comparator-based columnar sort, a bounded heap, and deferred normalized keys

## Status

Accepted, as-built for P6 (SORT / LIMIT / TOP-N).

## Context

P6 adds three related operators: `ORDER BY` (Sort), `LIMIT` (a row cap), and the fused `ORDER BY ... LIMIT k` (Top-N). All three must slot into the push-based, vectorized, columnar architecture fixed by the prior ADRs:

- **Push-based execution (0001).** The executor owns the loop and drives chunks from Source to Sink. Sinks are pipeline breakers. An early-termination "stop pushing upstream" signal was acknowledged but explicitly deferred. The operator interface is `Consume(const DataChunk&)` per chunk plus `Finalize()`; a pipeline-breaking sink does its real work in `Finalize()`. Sort is the canonical pipeline breaker: it cannot emit anything until it has seen the last input row, because the smallest input row could arrive last.
- **Borrowed chunks (0006).** Chunks handed to a Sink are borrowed — they are valid only for the duration of the `Consume()` call and may be overwritten or freed afterward. Any operator that must retain data across calls is required to deep-copy. A full sort retains *all* rows, so it must copy them all.
- **ColumnarTable / 2048-row chunks (0007).** Data lives in column-major chunks of at most `kVectorSize = 2048` rows. This power-of-two size is what makes a global row index decompose into `(chunk, offset)` cheaply.
- **Validity bitmask (0003), German strings (0004), expression/3VL semantics (0009).** Sorting must honor SQL NULL ordering and per-type comparison, including the 16-byte German-string layout where VARCHAR is compared by bytes.
- **The SIMD ceiling (0008).** Branchy, type-dispatched, random-access code is memory-bound, not lane-bound. A comparator that branches per key per row, and a final gather that scatters reads across materialized storage, both live under that ceiling.

The design question for P6 is how to materialize, order, and bound rows correctly and defensibly, while being honest about which optimizations (early termination, normalized keys, the ADR 0004 string fast path, external sort) are deferred rather than absent by accident.

## Decision

### Sort (ORDER BY): materialize, sort indices, gather

Sort is a **pipeline-breaker Sink**. It operates in three phases.

**1. Materialize.** Because input chunks are borrowed (0006), Sort deep-copies every incoming row into its own storage during `Consume()`. It repacks the copied rows into the operator's own `kVectorSize`-sized chunks (`Sort::Append`) so that the materialized data is fully packed and contiguous in chunk units. With a fixed power-of-two chunk size, a global row index `idx` in `[0, N)` locates in O(1). The source does this with integer division and modulo by the `kVectorSize` constant:

```
chunk  = idx / kVectorSize     // sort.cpp
offset = idx % kVectorSize
```

Because `kVectorSize` is a power of two, the compiler lowers these to a shift (`>> 11`) and a mask (`& 2047`); the source contains `/` and `%`, and that is the as-built primitive the comparator and the gather both rely on.

**2. Sort indices, not rows.** In `Finalize()`, Sort allocates an index array `[0, 1, ..., N-1]` of type `std::vector<std::uint32_t>` and runs `std::stable_sort` over **the indices**, with a multi-key comparator (`CompareRows`) that, given two indices, locates each row (two O(1) `(chunk, offset)` decompositions) and compares the sort keys. The wide materialized rows are never moved during the sort — only **4-byte (uint32) indices** are permuted. This is the central efficiency decision: a sort does O(N log N) comparator calls and a comparable number of element moves; moving an index is cheap and fixed-width, whereas moving a row means copying every projected column. Only the final gather touches row data, exactly once per row.

Using `uint32` indices caps a single sort at ~4.29e9 rows; beyond that the index type (and the materialized-chunk addressing) would need widening to 64-bit. The cap is generous for the in-memory target and is stated rather than hidden.

**3. Gather.** Walking the sorted index array, Sort gathers rows in sorted order into fresh `kVectorSize` output chunks (via `CopyElement` per column) and pushes them downstream. The gather is the only phase that moves row payloads.

The comparator (`CompareRows`, shared with Top-N) supports:
- **Multiple sort keys**, compared left to right; the first key that produces a non-equal result decides, otherwise we fall through to the next key.
- **Per-key ASC/DESC.**
- **Per-key NULL ordering** via an explicit `nulls_first` flag.
- **Per-key, per-type value comparison.** Numerics (`int32`/`date`, `int64`, `double`, `bool`) compare by value. **VARCHAR compares via `std::string_view::operator<`** over each `StringRef`'s `.view()` — correct lexicographic order, shorter-as-prefix sorts first. (Implementation note: the comparator binds `Data<StringRef>()[r]` by reference, not the `Get<StringRef>` temporary, so an inlined short string's `.view()` does not dangle.)

**VARCHAR and ADR 0004.** ADR 0004 specifies a *sort-specific* fast path for German strings: compare the 4-byte prefix as a byte-swapped big-endian `uint32` first, then full bytes, then length as tiebreak. **The as-built sort does NOT use that fast path** — it does a plain `string_view` compare, which produces the correct ordering but forgoes the prefix optimization. The fast path is deferred, exactly like normalized keys; this ADR does not claim the 0004 ordering optimization is in play.

`std::stable_sort` is chosen so that rows with equal keys retain their input (insertion) order — stable behavior where required, e.g. when an outer `ORDER BY` only partially orders rows or when downstream logic assumes a deterministic tie order. The cost is explicit: `std::stable_sort` allocates an O(N) temporary buffer (degrading to a slower in-place merge with ~N·log²N comparisons only if allocation fails). Crucially, the array being sorted is the **index array**, so that temporary is **N 4-byte indices, not N rows** — small relative to the materialized payload. We accept that cost in exchange for guaranteed stability and the simpler mental model.

#### NULL-ordering semantics (precise)

NULL ordering is governed by one rule, stated carefully because it is a common interview trap:

- **`nulls_first` is an absolute output position.** If `nulls_first` is true for a key, NULLs sort to the front of that key's ordering; if false, to the back. This decision is **independent of ASC/DESC**.
- **ASC vs DESC flips only the non-null *value* comparison.** It does **not** flip the null-vs-non-null decision. A NULL does not "become large" under DESC; its placement is whatever `nulls_first` says.

Concretely, for a single key `CompareRows` decides in this order (sort.cpp):
1. If both values are NULL → equal on this key (fall through to next key).
2. If exactly one is NULL → the NULL goes first iff `nulls_first`, last otherwise — *regardless of ASC/DESC*.
3. If neither is NULL → compare values; reverse the result iff DESC.

This is **Strata's chosen model, and it matches DuckDB's *default*: NULLS LAST for both ASC and DESC** (i.e. `nulls_first = false` is the default, verified against DuckDB docs). DuckDB additionally offers *direction-relative* null modes via `PRAGMA default_null_order` (`NULLS_FIRST_ON_ASC_LAST_ON_DESC`, `NULLS_LAST_ON_ASC_FIRST_ON_DESC`); Strata models only the absolute mode, which is the default the validation oracle uses. So "absolute, independent of ASC/DESC" is our model matching DuckDB's default — not a universal law of SQL.

### Limit: streaming prefix, no early stop

`LIMIT n` is a **streaming Sink** (not a pipeline breaker). It maintains a running count (`emitted_`) of rows already forwarded. For each input chunk it forwards the prefix needed to reach `n` (copying that prefix of each column into an output chunk via `CopyColumn`) and drops the remainder; once `n` rows have been forwarded, subsequent chunks are dropped entirely (`Consume` returns early).

**Early termination is not implemented.** Telling the source to stop scanning once `n` rows are produced is exactly the deferred push "stop" signal from ADR 0001/0006. Without it, a bare `LIMIT` is **correct but not optimal**: it still consumes (and the upstream still produces) the entire input even after the cap is reached. We state this honestly; it is a known, bounded inefficiency, not a bug.

### Top-N (ORDER BY ... LIMIT k): a bounded max-heap

When `ORDER BY` is immediately bounded by `LIMIT k`, a full sort is wasteful — we never need more than `k` rows in order. Top-N is a pipeline-breaker Sink built on a **bounded binary max-heap of size k**, keyed by the sort order:

- Candidate rows live in a single `DataChunk kept_` of capacity `k`; the heap (`std::vector<std::uint32_t>`) holds *slot indices* into `kept_`. The heap comparator deems slot `a` "less" than slot `b` when `a` sorts *before* `b`, so `std::*_heap` keeps the **greatest = worst kept row at the root** — the current candidate for eviction.
- **Per input row (`Consume`):**
  - If fewer than `k` rows are currently kept, write the row into the next free slot, push it, and sift up.
  - Otherwise, compare the row against the root. If `CompareRows(new, root) < 0` — i.e. the new row is **strictly better** than the current worst — pop the root, **overwrite that slot's storage** with the new row, and re-push (sift down). If it is not strictly better, discard it.
- **At `Finalize()`**, the kept rows occupy slots `[0, count_)`; we `std::stable_sort` an order array of those slot indices with the same comparator and emit them in `kVectorSize` batches.

This is **O(N log k)** time and **O(k)** memory, versus the full sort's **O(N log N)** time and **O(N)** memory. For small `k` against large `N` — the common shape of `ORDER BY ... LIMIT 10` — the win is large in both dimensions, and Top-N never materializes the whole input.

**As-built constraint on k.** `kept_` is one `DataChunk` initialized to capacity `k` (`top_n.cpp`), and a `DataChunk`'s documented invariant is `size <= capacity <= kVectorSize` (data_chunk.hpp). So the current Top-N **assumes `k <= kVectorSize` (2048)**; a larger `k` would need multi-chunk candidate storage. The target `LIMIT` shapes (10, 100) sit well inside this; the constraint is recorded rather than silently relied upon.

#### Why Top-N equals full-sort-then-LIMIT k (the real argument)

Top-N uses the **exact same comparator** as Sort. Sharing the comparator is necessary but *not by itself sufficient* for equivalence; the equivalence holds because of three concrete as-built details:

1. **Eviction requires strictly-better:** the replace test is `CompareRows(new, root) < 0` (top_n.cpp). A row that merely *ties* the worst kept row is **discarded**, so an earlier-arriving equal row is never displaced by a later equal one — matching the stable-sort tie rule (earlier input wins).
2. **Tied survivors are never evicted**, by (1), so a kept row that ties another keeps the slot it was first written into.
3. **The final survivor sort is stable over slot indices `[0, count_)`**, and because tied top-k rows retain their original (arrival-ordered) slots, slot order reproduces input order among ties — the same tiebreak full-sort-then-LIMIT would produce.

If eviction used `<=` instead of `<`, or if the final tiebreak were not stable, the equivalence would break. **This equivalence is exercised by the test suite**: `test_top_n.cpp` runs Top-N and a full `Sort` over the same randomized input for both ASC (`TopN.EqualsFullSortPrefix`, N=5000, k=17) and DESC (`TopN.DescEqualsFullSortPrefix`, N=3000, k=25) and asserts the k Top-N outputs equal the first k full-sort outputs position-for-position. (Scope, stated honestly: these tests cover a single `int32` key and compare *values* at each position; they do not yet pin multi-key, multi-type, or cross-type tie-stability byte equivalence. Extending them to the full property argued above is a natural next test, not a claim made here.)

### Normalized keys (deferred — the next optimization)

The current comparator does a **per-pair, per-key, per-type, branchy** comparison: for each pair of indices it dispatches on column type, checks validity, branches on ASC/DESC and on `nulls_first`, and for multi-column sorts repeats this for each key until one breaks the tie. That is many branches and type-dispatches per comparison, run O(N log N) (or O(N log k)) times. It is not SIMD-friendly and not cache-friendly: each comparator call also performs two O(1) `(chunk, offset)` locates and chases the resulting pointers.

The known optimization — used by DuckDB and other column stores — is the **normalized (binary) sort key**: encode each row's sort keys **once**, up front, into a single fixed-layout, **order-preserving** byte string such that the entire multi-key comparison collapses to a single `memcmp`. The encoding sketch:

- **Signed integers:** flip the sign bit so that two's-complement ordering becomes unsigned byte-lexicographic ordering; store big-endian.
- **IEEE doubles:** transform into an unsigned-comparable form (for a non-negative double flip the sign bit; for a negative double flip all bits), so byte order equals numeric order; store big-endian. **Finite values only:** NaN has no defined position in this total order, and `-0.0`/`+0.0` have distinct bit patterns but compare numerically equal, so both must be canonicalized — and defined *consistently with the comparison path*, which uses `double operator<` (where NaN is unordered and `-0.0 == +0.0`). These IEEE edge cases are part of why normalized keys are deferred rather than rushed.
- **DESC keys:** invert all bits of that key's encoded **value** bytes, so a single ascending `memcmp` yields descending order.
- **NULL ordering:** prepend a one-byte null sentinel per key whose value places NULLs first or last as required by `nulls_first`. **The sentinel byte must sit OUTSIDE the DESC bit-inversion** — the inversion applies only to the value bytes — otherwise inverting it for DESC would flip the null placement and contradict the absolute-`nulls_first` rule. (A single distinguishing byte cannot be made invariant under inversion, so excluding it from the inversion is the mechanism, not a choice of magic constants.) This sentinel/DESC interaction is the one genuinely subtle part of the scheme.
- **Strings:** store fixed-width, or as an order-preserving prefix with a **tiebreak path** (compare the full string only when prefixes are equal) — this is also where the deferred ADR 0004 byte-swapped-prefix protocol would naturally live.

The keys for all sort columns are concatenated in key order into one contiguous byte string per row. The whole comparison then becomes **one `memcmp` over contiguous bytes** — branch-free, vectorizable, cache-friendly — and, crucially, it is also what enables **radix / MSD sorting**, which can beat comparison sort entirely.

We **defer** normalized keys. The comparator is correct, much simpler to read and reason about, and adequate for the target query set; building order-preserving encoders for every type (the IEEE edge cases, the sentinel/DESC interaction, the string tiebreak path) is real work that buys nothing until comparison cost actually dominates. Normalized keys are the clear, named next step, not an oversight.

## Alternatives Considered

- **Sort the rows in place (move row payloads during the sort).** Rejected: every swap/merge step would copy wide, multi-column rows. Sorting fixed-width `uint32` indices and gathering once moves each row's payload exactly one time instead of O(log N) times.

- **`std::sort` instead of `std::stable_sort`.** `std::sort` is in-place (no O(N) temp) and typically faster by a constant, but it is **not stable** — equal-key rows could be reordered nondeterministically, breaking "stable where required" and making output validation against DuckDB fragile. We chose determinism and stability; the O(N) temporary (of *indices*) is the price. (A future normalized-key radix/MSD sort could revisit this, since a fully specified key removes ties.)

- **Streaming / non-materializing sort.** Impossible for a true global sort: the last input row can belong at the front of the output, so nothing can be emitted before the input is exhausted. Sort is inherently a breaker (0001).

- **External / spilling sort (sort larger-than-memory inputs by spilling runs to disk and merging).** Deferred. Strata's target inputs fit in memory; an in-memory sort is simpler and faster there. We note the limitation rather than build the machinery.

- **Full sort for `ORDER BY ... LIMIT k`.** Rejected in favor of Top-N: a full sort is O(N log N) time and O(N) memory; the bounded heap is O(N log k) time and O(k) memory and gives identical output (argued above, exercised by the int32 ASC/DESC equivalence tests).

- **Quickselect / partial sort for Top-N.** A `nth_element`-style partition could find the top `k` in O(N) average time, but it requires the full input materialized first (it partitions in place over all N rows), giving up the heap's O(k) memory and its streaming, one-pass-over-borrowed-chunks behavior. The bounded heap keeps only `k` rows live at any time, which matters precisely when `k ≪ N`.

- **Normalized keys now.** Rejected for P6 on cost/benefit grounds (see above) — deferred, with the encoding specified (including the IEEE and sentinel/DESC caveats) so it can be picked up directly.

- **ADR 0004 byte-swapped-prefix string fast path now.** Deferred alongside normalized keys; the plain `string_view` compare is correct, and the prefix fast path buys nothing until string comparison cost dominates.

- **Early-termination signal for `LIMIT` now.** Rejected: it is the deferred upstream "stop" signal from 0001/0006 and touches the executor's control-flow contract, not just the Limit operator. Building it here would pre-empt that broader decision.

## Consequences

**Wins**
- Sort moves each row's payload exactly once (final gather); the sort itself permutes only cheap 4-byte indices.
- `std::stable_sort` gives deterministic, stable output that validates cleanly against DuckDB; its temporary is N indices, not N rows.
- O(1) `(chunk, offset)` locate from a global index, courtesy of the power-of-two chunk size (div/mod lowered to shift/mask).
- Precise, independent control of ASC/DESC and NULLS FIRST/LAST per key; default null order matches DuckDB's default (NULLS LAST, both directions).
- Top-N is O(N log k) time / O(k) memory and never materializes the full input; it shares one comparator with Sort, and its equivalence to full-sort-then-LIMIT (under strictly-better eviction + stable final sort) is exercised by the int32 ASC/DESC equivalence tests.
- `LIMIT` is a cheap streaming prefix copy.

**Tradeoffs (stated honestly)**
- **Comparator branchiness.** The per-pair, per-key, per-type, ASC/DESC, NULL-flagged comparison is branchy and not SIMD-friendly — the kind of code that lives under the memory-bound ceiling of 0008. Normalized keys are the fix and are deferred.
- **No ADR 0004 string fast path.** VARCHAR sorts via plain `string_view` compare; the byte-swapped-prefix protocol from 0004 is not wired in. Correct, not optimized.
- **Full materialization.** Sort copies and retains the entire input, so its memory is proportional to input size. There is no external/spilling sort: inputs larger than memory are not handled. Deferred, by design, for the in-memory target.
- **uint32 index ceiling.** A single sort/Top-N is capped at ~4.29e9 rows by the `uint32` index type; 64-bit widening would be required beyond it.
- **Top-N k ceiling.** `kept_` is a single `DataChunk` of capacity `k`, so the as-built Top-N assumes `k <= kVectorSize` (2048); larger `k` needs multi-chunk candidate storage.
- **Random-access final gather.** Emitting rows in sorted order reads the materialized storage in permuted (effectively random) order — random-access, memory-bound, again the 0008 ceiling.
- **Per-comparison locate cost.** Every comparator call performs two O(1) `(chunk, offset)` locates (cheap shift/mask, but non-zero and repeated O(N log N) times); normalized keys would replace this with a single contiguous `memcmp`.
- **`LIMIT` consumes all input.** With no early-termination signal, a bare `LIMIT` still drives the full upstream scan: correct, not optimal. Tied to the deferred push "stop" of 0001/0006.

## How to defend this at a whiteboard

- **Why is Sort a Sink that materializes everything?** Global sort is a pipeline breaker (0001) — the last input row can sort first, so nothing emits until input is exhausted. And chunks are borrowed (0006), so retaining all rows means deep-copying all of them. I repack copies into my own 2048-row chunks so a global index decomposes in O(1): `chunk = idx / kVectorSize`, `offset = idx % kVectorSize`, which the compiler lowers to `>> 11` / `& 2047` because the chunk size is a power of two.
- **Why sort indices instead of rows?** Sorting permutes only 4-byte `uint32` indices; the wide row payloads never move during the sort. Only the final gather moves each row's data, exactly once — versus O(log N) moves per row if I sorted the rows themselves. (uint32 caps me at ~4.3 billion rows; I'd widen the index to go past that.)
- **Why `std::stable_sort`?** Equal-key rows must keep input order (stable where required) and output must be deterministic for validation against DuckDB. The price is an O(N) temporary — but it's N indices, not N rows, so it's small; `std::sort` would avoid even that but isn't stable.
- **State the NULL rule exactly.** `nulls_first` is an absolute position and is independent of ASC/DESC; ASC/DESC flips only the comparison of two non-null values, never the null-vs-non-null decision. That's my model and it matches DuckDB's *default* (NULLS LAST both directions). DuckDB also has direction-relative null modes via `PRAGMA`; I model only the absolute one, which is what my oracle uses.
- **How does Top-N work?** A size-`k` max-heap whose root is the *worst* kept row. Per input row: insert if fewer than `k`; otherwise if the row is *strictly* better than the root, overwrite that slot and sift down. O(N log k) time, O(k) memory, never materializes all N rows. At Finalize, stable-sort the `k` survivors and emit. (As built, `k` must be ≤ 2048 because candidates live in one DataChunk.)
- **Why is Top-N trustworthy — isn't "same comparator" hand-waving?** No: equivalence to full-sort-then-LIMIT holds because eviction is *strictly-better-only* (`< 0`), so tied rows are discarded and never evict an earlier equal row, and the final survivor sort is *stable over arrival-ordered slots*. That reproduces stable-sort-then-limit's tie semantics. The int32 ASC and DESC cases are checked against a full Sort in the test suite; I'd extend that to multi-key/multi-type to pin it fully.
- **Why does a bare `LIMIT` still read everything?** I forward the first `n` rows and drop the rest, but I don't tell the source to stop — that's the deferred upstream "stop" signal from 0001/0006. Correct, not optimal; I'd add the stop signal to fix it.
- **What's the next optimization?** Normalized (binary) sort keys: encode each row's keys once into one order-preserving byte string — sign-bit-flip integers, transform doubles to unsigned-comparable form (finite-only; canonicalize NaN and ±0.0 to match the compare path), invert *value* bytes for DESC while keeping the null sentinel *outside* the inversion, fixed-width or prefix+tiebreak for strings — so the whole multi-key compare becomes a single `memcmp`. That's branch-free, vectorizable, cache-friendly, and unlocks radix/MSD sort. DuckDB does this; I deferred it (and the ADR 0004 string prefix fast path) because the comparator is correct, simpler, and adequate for the target query set.
