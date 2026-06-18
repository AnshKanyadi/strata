# 0003. Validity (NULL) Representation: a 1=valid Bitmask with an All-Valid Fast Path

## Status

Accepted.

## Context

SQL is three-valued: every value in every column is either a concrete value or `NULL`. The data plane (P1) must therefore carry per-row nullability alongside the values in every `Vector`, and every operator (filters, projections, joins, aggregates) must respect and propagate it. This decision is on the critical path of correctness *and* performance: it is touched by literally every row that flows through the engine.

Two forces pull in opposite directions:

1. **Most analytical columns have no NULLs.** TPC-H is a representative example: the overwhelming majority of columns are declared `NOT NULL` or are simply never null in practice. Any per-row NULL bookkeeping we pay on these columns is pure overhead against the common case.
2. **When NULLs *are* present, the representation must be cheap to test, cheap to combine, and SIMD-friendly** — three-valued logic has to compose across operators without branching per row where avoidable.

The representation also has to fit the columnar, vectorized model: values live in contiguous typed arrays of `kVectorSize = 2048` values, and we want NULL handling to be expressible as bulk bitwise operations over the batch, not as per-value scalar checks.

This is, at the framing level, the MonetDB/X100 insight (Boncz, Zukowski, Nes, *MonetDB/X100: Hyper-Pipelining Query Execution*, CIDR 2005): once data is columnar and processed a vector at a time, auxiliary per-row information should be a separate, dense, vectorizable side-channel rather than interleaved with the values. **Important honesty note:** X100 is the lineage for the *columnar, vectorized, dense-side-channel* framing, **not** for the specific NULL representation chosen here. MonetDB classically represented NULLs with per-type **NIL sentinel values** — a reserved value of the domain — which is precisely the approach this ADR rejects below. The 1=valid *bitmask* with an all-valid fast path descends instead from the **Apache Arrow / DuckDB validity-bitmap** lineage, and we attribute it there.

## Decision

Every `Vector` owns a `Validity` (see `include/strata/data/validity.hpp`, `src/data/validity.cpp`). The as-built design:

- **Representation: a flat array of `uint64` words, one bit per row.** Value `i` lives in word `i >> 6`, bit `i & 63`. Storage is a `std::vector<uint64_t>` **owned by the `Validity`** (RAII); no manual `free`, no aliasing of foreign storage.

- **Convention: bit set (1) = VALID / not-null; cleared bit (0) = NULL.** The 1=valid bit convention matches DuckDB's `ValidityMask`. (The *mechanism* by which we represent all-valid — an empty `std::vector` — is Strata's own; DuckDB represents all-valid via a null validity *pointer*, and Arrow via an optional/absent validity buffer. The claim of parity with DuckDB is scoped to the bit convention only.)

- **No allocation at construction.** `Validity()` default-constructs an empty mask with capacity 0. `explicit Validity(std::size_t capacity)` *records* the capacity but performs **no heap allocation** — the `std::vector` is genuinely empty (no buffer). The word count it will use when it eventually allocates is `WordCount(capacity) = (capacity + 63) / 64`, a compile-time-shaped constant (32 words for `kVectorSize = 2048`). The 32-word figure is the *target allocation size used at first NULL*, not a reservation made up front.

- **The mask is allocated lazily — on the first NULL.** A freshly constructed `Validity`, and any `Validity` that has never recorded a NULL, holds an empty buffer. `AllValid()` returns true iff that buffer is empty (`mask_.empty()`). This is the load-bearing predicate: operators call it once per vector; if true, they take a **null-check-free fast path** over the whole batch.

- **Helpers (as built — exact signatures):**
  - `bool AllValid() const` — returns `mask_.empty()`. O(1).
  - `bool RowIsValid(std::size_t i) const` — if the mask is empty, returns `true` unconditionally; otherwise `((mask_[i>>6] >> (i&63)) & uint64_t{1}) != 0`. (The shift operands are `uint64_t`, so there is no shift-width UB; a right-shift-and-mask of a 64-bit word by `0..63` is well-defined.)
  - `void SetInvalid(std::size_t i)` — ensures the mask exists, then clears bit `i`: `mask_[i>>6] &= ~(uint64_t{1} << (i&63))`.
  - `void SetValid(std::size_t i)` — ensures the mask exists, then sets bit `i`: `mask_[i>>6] |= (uint64_t{1} << (i&63))`.
  - `void Reset()` / `void Reset(std::size_t capacity)` — drop the mask buffer (`mask_.clear()`), returning to the unallocated, all-valid fast-path state (optionally re-setting capacity). **This is the only way back to the fast path** once a mask has been allocated; there is no `SetAllValid` re-tightening (see Consequences).
  - `std::size_t CountValid(std::size_t count) const` — see tail-masking below.
  - `const uint64_t* data() const` — the raw words, or **`nullptr` when all-valid** (mask empty). Exposed for later SIMD mask-combining kernels.
  - `static constexpr std::size_t WordCount(std::size_t n)` — `(n + 63) / 64`.

  The private `EnsureAllocated()` is the heart of the lazy path: on first need it does `mask_.assign(WordCount(capacity_), ~uint64_t{0})` — i.e. it materializes the buffer **all-ones (all valid)** before any bit is cleared.

### `CountValid` tail semantics (stated explicitly, not hand-waved)

`CountValid(count)` returns the number of valid rows among the first `count`:
- **All-valid fast path:** if the mask is empty, return `count` with no memory access.
- **Populated path:** `std::popcount` each *full* 64-bit word (`count >> 6` of them), then for the partial tail (`rem = count & 63`, if nonzero) popcount only the low `rem` bits of the final word: `mask_[full_words] & ((uint64_t{1} << rem) - 1)`.

The tail mask is necessary because the allocated buffer is `WordCount(capacity)` words initialized to all-ones, so bits *beyond* `count` read as VALID and a naive popcount over all words would overcount. Note `2048 % 64 == 0`, so a *full* vector has no partial last word; the tail mask matters specifically for **partial chunks** (the common final batch of a scan). `count` is therefore a required argument, not optional. (`std::popcount` from `<bit>` is available and used in this environment.)

### Why a bitmask, not a byte-per-value flag array

- **8x denser.** One bit vs. one byte per row. For 2048 rows the mask is 256 bytes — four cache lines' worth of footprint — versus 2 KiB for a byte array. (A `std::vector` allocation is not guaranteed 64-byte-aligned, so 256 B can in the worst case straddle five lines; the point is that it is tiny and trivially L1-resident, not that the line count is a hard guarantee.)
- **Cache-friendly.** Density translates directly into fewer cache lines touched per batch, the dominant cost in a vectorized engine.
- **Combinable with bitwise ops.** Two masks combine with a single `AND` over `uint64` words (32 word-ops for a full vector); `std::popcount` counts set (valid) bits, and `std::countr_zero(word)` finds the next valid row while `std::countr_zero(~word)` (equivalently `countr_one(word)`) finds the next NULL row. A byte array forces per-element work for all of these.

### Why "1 = valid" specifically

- **The decisive reason — strict-function NULL propagation is a bitwise AND.** For a **strict** scalar function (one where any NULL input forces a NULL output — arithmetic, comparisons, most scalar built-ins), a result row is valid iff *all* of its inputs are valid. With 1=valid that is exactly `result_mask = mask1 & mask2 & ...`: a word-parallel AND, and the all-valid fast path falls out for free (if every input is unallocated/all-valid, so is the output, with zero work). This is the genuine discriminator between 1=valid and 0=valid — under 0=valid the same composition would need an OR plus inverted reasoning, and the empty-mask fast path would not compose as cleanly. The 1=valid `&` is the algebraic reason the convention is not arbitrary.
- **All-ones init is the *implementation detail* that makes 1=valid zero-cost, not an independent reason.** `EnsureAllocated()` materializes `~uint64_t{0}` so that "newly materialized, nothing-cleared-yet" means all-valid, and conceptually the *absent* mask is the all-ones mask. (Lazy allocation — absent mask = all-valid — would work under either convention, so it is not what distinguishes 1=valid; the `&` composition above is.) There is no zero-filled mask anywhere: a zero-filled mask would mean *all NULL* under 1=valid, which is never used.

**Scope of the AND claim (carve-out for P3).** `result_mask = mask1 & mask2` is correct **only for strict functions**. It is *not* the rule for the non-strict cases, whose output validity depends on input *values*, not just input validity:
- **Boolean connectives** are non-strict: `FALSE AND NULL = FALSE` (a valid result from a NULL input), `TRUE OR NULL = TRUE`. Validity here is value-dependent.
- **`COALESCE` / `IFNULL` / `CASE`** are non-strict by design.

These will be handled specially by P3's three-valued-logic layer, where a `WHERE` predicate passes only rows evaluating to **TRUE** (filtering out both **FALSE** and **NULL**). This ADR commits the validity *representation* and the strict-function composition rule; it does not claim a blanket "all NULL propagation is an AND."

### The all-valid fast path

This is the single most important correctness-and-performance lever in the validity design. Because most analytical columns have no NULLs, `AllValid()` is true for most vectors, and when it is, every operator skips *all* per-row null checks for that batch — no bit tests, no branches, no extra cache lines. NULL handling for the common case costs one O(1) predicate per vector and zero per-row work. The fully-correct per-row path exists and is taken only when a mask is actually present.

## Alternatives Considered

- **Sentinel-value NULLs (reserve one value of the domain to mean NULL — the classic MonetDB NIL approach).** Rejected. There is no spare sentinel across a full integer range — every bit pattern of an `int32`/`int64` is a legitimate value. It pollutes SIMD arithmetic (every kernel would have to special-case the sentinel, defeating the point of vectorization), it is type-specific (no uniform story for strings, decimals, dates), and it conflates "value" with "absence of value". (This is exactly what MonetDB historically did and is the reason we do *not* claim the bitmask descends from X100's NULL handling.)

- **A separate boolean/byte column per value (one byte = null flag).** Rejected. 8x the space of a bitmask and correspondingly worse cache behavior, and it gives up the cheap bitwise composition (`AND` of two masks) that strict-function three-valued logic wants. It is strictly dominated by the bitmask. (Note: such a byte column could be an owned `std::vector<uint8_t>` with identical RAII guarantees — so RAII is *not* a discriminator here; density and bitwise composition are.)

- **Per-value `std::optional<T>` (or any tagged-per-element type).** Rejected. It defeats the columnar layout entirely: values are no longer contiguous typed arrays, so no SIMD kernel can run over them, alignment and padding balloon the footprint, and the engine loses the entire X100/Photon vectorization premise. This is fundamentally a row-store representation of nullability.

## Consequences

**Wins:**
- The common case (no NULLs) costs **one O(1) `AllValid()` check per vector** and zero per-row overhead, plus zero heap allocation for the mask.
- When NULLs exist, the mask is dense (256 B/vector, trivially L1-resident) and composes via word-parallel bitwise `AND` for strict functions; counting is `std::popcount`, next-valid-row scanning is `std::countr_zero`, next-NULL-row scanning is `std::countr_zero(~word)`.
- The 1=valid convention makes strict-function NULL propagation algebraically clean (`mask1 & mask2`) and is consistent with DuckDB's bit convention, easing validation against it.
- **Separately:** ownership is RAII via an *owned `std::vector<uint64_t>`* inside the `Validity` — no leaks, no manual lifetime management, no foreign-buffer aliasing. (This is a property of choosing owned `std::vector` storage; it is orthogonal to the bit-vs-byte and 1-vs-0 choices, and would hold for a byte-array design too.)

**Tradeoffs accepted (honestly):**
- The validity mask is a **separate side-channel that must be threaded through every operator.** Every operator that produces output must decide and set the output's validity; this is real, recurring plumbing, not free. The **non-strict cases (boolean AND/OR, COALESCE, CASE) are extra work P3 must implement specially** — they are not covered by the simple `&` rule.
- When NULLs are present, the per-row **bit test `((mask_[i>>6] >> (i&63)) & uint64_t{1})` costs** on the slow path, and predicate/expression operators must branch or mask accordingly.
- **Allocation is coarse and sticky: the first `SetInvalid` allocates the whole 32-word mask**, even for a single NULL. And once allocated, **`AllValid()` does not re-tighten to `true`** even if every cleared bit is later set back to valid via `SetValid` — we do not scan the mask to reclaim the fast path. `Reset()` is the explicit way back to the unallocated all-valid state. This is a deliberate simplicity-for-speed choice: we never pay to *detect* that a column became all-valid again mid-pipeline.

## How to defend this at a whiteboard

- "One bit per row, 1 means valid, lazily allocated. No mask means no NULLs means skip every null check — that's the whole game, because analytical columns are mostly NOT NULL. `AllValid()` is just `mask_.empty()`, O(1)."
- "Why 1=valid? The real reason is composition: for **strict** functions a row is valid iff all inputs were, so `result = mask1 & mask2` — a word-parallel AND, and all-valid composes for free. All-ones init (`EnsureAllocated` writes `~0ull`) is just the implementation detail that makes that convention zero-cost; it's not the reason by itself, since lazy allocation works under either convention. Bit convention matches DuckDB's `ValidityMask`."
- "Scope check, before someone catches me: the AND rule is **strict functions only**. Boolean AND/OR are non-strict — `FALSE AND NULL = FALSE`, `TRUE OR NULL = TRUE` — and so are COALESCE/CASE; their output validity depends on values, not just input masks. P3's TVL handles those specially; a `WHERE` predicate passes only TRUE, dropping FALSE and NULL."
- "Why a bitmask over a byte array? 8x denser — 256 bytes for 2048 rows, four cache lines' worth, lives in L1 — and it composes with bitwise AND, counts with `popcount`, scans with `countr_zero` (next valid) or `countr_zero(~word)` (next NULL). A byte array buys nothing on those and costs cache. RAII is *not* the discriminator — a `vector<uint8_t>` is equally RAII; density and bitwise composition are."
- "Why not sentinels? No free value in a full integer domain, it poisons SIMD, and it's type-specific — and that's actually what classic MonetDB did for NULLs, which is why I cite X100 for the *columnar/vectorized side-channel framing* but **not** for this NULL representation. The 1=valid bitmask-with-fast-path lineage is Arrow/DuckDB validity bitmaps. Why not `std::optional` per value? That's a row store — kills contiguous arrays and SIMD, the entire point of X100/Photon-style execution."
- "No shift-width UB: the test is a right-shift of a `uint64_t` by 0..63 then mask — `(mask_[i>>6] >> (i&63)) & 1ull`. And `SetInvalid` clears with `~(uint64_t{1} << (i&63))`. The literals are 64-bit, so nothing shifts past its width."
- "No allocation at construction — `Validity(capacity)` only records the capacity; the `std::vector` stays empty until the first `SetInvalid`, which allocates `WordCount(2048)=32` words all-ones. `CountValid(count)` takes the row count and tail-masks the partial last word, because beyond `count` the all-ones init reads as valid and a naive popcount would overcount — matters for partial final chunks since `2048 % 64 == 0`."
- "Honest costs: I thread the mask through every operator; the non-strict TVL cases are extra work in P3; on the slow path I pay a per-row bit test; the first NULL allocates the whole 32-word mask; and I don't re-tighten back to the fast path if NULLs are later overwritten — `AllValid()` stays false until an explicit `Reset()`. I traded that laziness for never scanning to reclaim the fast path."
