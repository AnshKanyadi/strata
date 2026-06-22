# 0010. Aggregate hash table: open addressing, linear probing, salt, and a row layout

## Status

Accepted — **implemented as-built in P4**.

This ADR was drafted in parallel with the implementation, so the body still uses
an "intended"/"will" voice and flags some paragraphs "(as-built)". As of this
commit the **entire** design below is built and shipped: the `HashAggregate` sink
(`include/strata/exec/hash_aggregate.hpp`, `src/exec/hash_aggregate.cpp`) — open
addressing, the slots directory + salt, the hash stored in the row, the row
buffer `[ hash | keys | null-flags | states ]`, growth/re-slotting by stored
hash, the per-insert `StringHeap` copy for VARCHAR keys, the linear-probe loop,
the `gidx[]` scatter-update, and the global (no-key) single-group driver — plus
the state kernels resolved by `ResolveAggregate`. It is covered by 8
GROUP-BY/global/NULL/overflow tests and cross-checked against DuckDB. Read every
"will"/"intended" below as "does", and ignore the now-obsolete "not yet
implemented" caveats.

## Context

P4 adds `GROUP BY` with `COUNT(*)`, `COUNT(col)`, `SUM`, `MIN`, `MAX`, `AVG`, and the global (no-`GROUP BY`) forms of the same aggregates. A `GROUP BY` is a pipeline breaker (ADR 0001): the aggregating `Sink` must consume the *entire* probe-side input, accumulating one running aggregate state per distinct group, before it can emit a single output row. The central data structure is therefore a **hash table keyed on the grouping columns, whose payload is the running aggregate state for that group** — built incrementally as batches arrive, drained once at the end.

This will be the hottest random-access structure in the engine. Per ADR 0008's ceiling, hash-table probing is **gather / pointer-chasing — memory-latency bound, not lane-bound** — so the design lever that matters is not SIMD width but *cache behavior and instructions-per-probe*: how many cache lines a probe touches, how cheaply a non-matching slot is rejected, and whether the structure allocates per group. Those are the questions this ADR answers.

**What exists as of P4 (as-built):** the state kernels resolved by `ResolveAggregate(AggFunc, TypeId)`. Each `(func, input-type)` pair resolves to a `ResolvedAggregate { state_size, output_type, reads_input, update(...), finalize(...) }`. The `update` runs **once per batch** (it loops internally over the batch, no per-row dispatch), folding input value `v[i]` into the state of group `gidx[i]`; `finalize` produces the group's result. The state structs (`CountSt`, `SumSt<A>`, `MinMaxSt<T>`, `AvgSt<A>`) are designed to live **inline in a group row** and are **zero-initialized** at group creation (zero == correct initial state: `count 0`, `sum 0`, `has=false`), so there is no separate init step. The hash table that *creates* those rows, computes the `gidx[]`, and drains them is what this ADR proposes.

The constraints that shape the layout:

- **Up to 2048-row batches** (`kVectorSize`, ADR 0007) arrive at the sink; we want to amortize per-row interpretation overhead across the batch exactly as the rest of the engine does (ADR 0008/0009), so the unit of work is a batch, not a row. The state kernels already honor this (one internal loop per `update`).
- **Multi-key `GROUP BY`** (e.g. `GROUP BY l_returnflag, l_linestatus`) must be a first-class case, not a special one.
- **NULL is a legal group key.** `GROUP BY x` with NULLs in `x` must produce one group for the NULLs, intended to match SQL / DuckDB: `NULL` groups *with* `NULL`, and is distinct from every non-NULL value.
- **VARCHAR keys** (`GROUP BY n_name`) are 16-byte `StringRef`s (ADR 0004) whose long-string bytes live in the *input batch's* `StringHeap`. That batch is borrowed and is gone after the `consume()` call (ADR 0006/0007), so a string key retained in the table must not point into it.
- **No exceptions in the hot path** (ADR 0002); growth and insertion will be branch-predictable, allocation-checked code, not throwing code.

## Decision

### Open-addressing, linear-probing table with a row-layout payload (intended)

The aggregate hash table will be **open-addressed with linear probing**, split into two arrays:

1. **A row buffer** — one contiguous, growable byte buffer holding the **group rows**. Each group is one row appended at a stable index; a group, once created, never moves within the buffer (only the slots array is rebuilt on growth). A row is a packed, fixed-width record:

   ```
   [ uint64 hash | key values (packed, per-key physical width)
                 | key null-flags (1 byte per key)
                 | aggregate states (packed) ]
   ```

   - **`uint64 hash`** — the full hash of the key, *stored in the row* (see "Why store the hash").
   - **key values** — each key column's value at its physical width (`int32` = 4B, `int64`/`double` = 8B, VARCHAR = a 16-byte `StringRef`, ADR 0004), packed in key order.
   - **key null-flags** — one byte per key column, the NULL bit lifted out of the validity bitmask and into the row so the key (value + null-ness) is self-contained and comparable in place.
   - **aggregate states** — the running accumulator for each aggregate, packed, at `state_offset`. **(as-built — the state structs)** these are exactly the kernels' structs: `CountSt { int64 count }`, `SumSt<A> { A sum; bool has }`, `MinMaxSt<T> { T value; bool has }`, `AvgSt<A> { A sum; int64 count }` (see "Aggregate state").

2. **A slots array** — the open-addressing directory, sized to a power of two. Each slot is small and holds:
   - a **group index** into the row buffer (a reserved sentinel value means **empty**), and
   - a **salt**: the top bits of the key's hash, cached in the slot for fast rejection.

   The slot is intentionally *not* the group row; it is a compact (index, salt) pair so the directory stays dense and cache-friendly and growth rewrites only the directory.

The defining property: **a group's key and its aggregate state will live together in one row.** A probe that finds its group dereferences *one* row region — keys to confirm the match, states to update — touching one contiguous cache-line span, not two independent structures.

### The probe (per batch, then per row within the batch) — intended

For each input batch the sink will:

1. **Hash the key column(s)** for all rows in the batch. Multi-key hashing combines the per-key hashes (order-sensitive mix) into one `uint64` per row.
2. For each row, **linear-probe** the slots array starting at `slot = hash & mask` (`mask = capacity - 1`, capacity a power of two):
   - **Empty slot** (sentinel group index): the key is new. **Append a new group row** to the row buffer (write the stored hash, the packed key values, the per-key null-flags; states are left zero-initialized, the correct initial state), and write `(group_index, salt)` into this slot. Probe ends.
   - **Salt mismatch**: the slot's cached salt differs from this key's salt — it *cannot* be our group. **Skip to the next slot** (`slot = (slot + 1) & mask`) **without dereferencing the row buffer at all.** This is the common rejection and it costs a single byte/word compare against data already in the slot's cache line.
   - **Salt match**: a *candidate*. Dereference the group row and do a **full key compare** (all key columns, value and null-flag, per the equality protocol below). Equal ⇒ found, probe ends. Unequal (a salt collision / hash collision) ⇒ continue linear probing.

The result of the probe phase is a **per-row group-index array** `gidx[]` (`std::uint32_t` per row): row `i` of the batch maps to group index `gidx[i]`. **(as-built)** this array is exactly the hand-off the state kernels already consume.

### Update: probe once, scatter-update per aggregate

Aggregation **dispatches once per batch, per aggregate** — the same amortization discipline as the expression executor (ADR 0009) and the kernels (ADR 0008). **(as-built — the kernels)** after the probe produces `gidx[]` for the whole batch, **each aggregate's `update` runs one internal loop over the batch**, reading input value `v[i]` (and its validity) and folding it into the state of group `gidx[i]`:

```
for each aggregate a:
    a.update(rows, row_width, a.state_offset, gidx, col, n)   # one pass per aggregate per batch
        # inside: for i in [0,n): if col valid -> a.combine(state_of(gidx[i]), v[i])
# COUNT(*) increments unconditionally (it counts rows, not non-null values)
```

**Be honest about the shape of `state_of(gidx[i])`: it is a random-access scatter** into the row buffer indexed by `gidx[i]` (`rows + gidx[i] * row_width + state_offset`, exactly as `StateAt` computes it as-built). Consecutive batch rows can map to arbitrary, non-contiguous groups, so the state writes are gather/scatter, not a streaming store.

**The honest, *qualified* claim about SIMD here:**

- For **high-cardinality grouping** (many distinct groups, the general case), the scatter targets group state lines spread across a large row buffer with data-dependent addresses. There is **no native gather/scatter on NEON**, and the loop is bound by **memory latency on the scattered state lines**, not by arithmetic lane width. This is a **poor SIMD target**, and we do **not** claim a SIMD speedup for it. The win is *not* vectorization — it is the per-batch dispatch amortization plus the cache locality of the row layout (the state we scatter into is the *same* row we already touched during the probe's key compare, so the line is frequently already hot).
- For **low-cardinality grouping** (e.g. `GROUP BY l_returnflag, l_linestatus` ≈ 4 groups) and for **global aggregation** (1 group, below), the picture is different and we say so: the few state lines stay resident in L1, the update is *not* latency-bound, and a vectorized / auto-vectorized per-group reduction **is** viable (DuckDB and others special-case low-cardinality and perfect-hash aggregation for exactly this reason). **We currently leave that optimization on the table** as a deliberate simplification: the as-built kernels run a plain scalar fold for all cardinalities. This is a known unexploited win, not a claimed one.

This is the same shape of honesty as ADR 0008's logical-connective and string paths: a deliberately scalar path, named as such, with its limits stated rather than papered over.

The two-phase split (probe whole batch → group indices → per-aggregate update loop) keeps each aggregate's inner loop simple and branch-predictable, and isolates the one unavoidable random-access step (the probe) from the value folding.

### Why store the hash in the row

The stored `uint64 hash` in each group row earns its 8 bytes at **growth**. When load factor crosses the threshold the slots array doubles, and every existing group must be re-placed into the new, larger directory. Re-slotting reads each group's **stored hash** and recomputes only `hash & new_mask` (and the salt) — it **never re-reads or re-hashes the keys**. This matters most for **string keys**: rehashing a VARCHAR means touching the inline prefix and possibly chasing the `StringHeap` pointer for the tail (ADR 0004) — exactly the latency-bound work we want to avoid doing twice.

**Be precise about what growth still costs.** Storing the hash avoids the *key re-hash*; it does **not** make growth a pure sequential pass. Growth is: a sequential read over the row buffer's hash fields, **plus** an O(groups) **re-insertion** that itself probes the new directory (open-addressing insert with linear probing per group). The new table is empty as we fill it, so those probe sequences are short, but they are random writes into the fresh directory, not free. The win is "no key re-hash," not "no re-insertion."

(The stored hash also lets the probe's salt come straight from the row on the rare full-compare path, and keeps the slot small — the slot caches only the salt, not the whole hash.)

### NULL group keys: null-as-its-own-group via a null-flag in the key (intended semantics)

A NULL is intended to be a legal, distinct group. The semantics we intend, matching DuckDB's grouping rules:

- **`NULL` groups with `NULL`** — all NULLs in a key column collapse into **one** group. (This is *grouping* equality, which differs from SQL's 3VL comparison where `NULL = NULL` is *unknown*; for `GROUP BY`/`DISTINCT`, SQL treats NULLs as **not distinct from each other**.)
- **`NULL` is distinct from every non-NULL value.**
- For multi-key groups, the null-ness of *each* key participates: `(NULL, 'A')`, `('x', NULL)`, and `(NULL, NULL)` are three different groups.

The mechanism is the **per-key null-flag byte in the row**. The key is the pair `(value, is_null)` per column. Key equality is:

- both null-flags set ⇒ keys equal *on this column* (NULL == NULL for grouping), regardless of the value bytes;
- one set, one clear ⇒ unequal;
- both clear ⇒ compare the value bytes (numeric equality, or the `StringRef` equality protocol of ADR 0004).

A row is a group match only if **every** key column matches under this rule. Lifting the NULL bit out of the validity bitmask (ADR 0003) and into a per-key byte in the row makes the stored key self-contained: equality and hashing operate on the row alone, with no side reference to a batch that has already been freed.

**This semantics is a design target, not a validated result.** There is no aggregation test or DuckDB diff harness in the tree yet. The claim "matches DuckDB on NULL grouping" must be earned by a test that diffs Strata `GROUP BY`-with-NULLs output against DuckDB before it can be stated as fact.

### Global (no-`GROUP BY`) aggregation: a single pre-created group, no hashing (intended)

A query with aggregates but **no** `GROUP BY` (`SELECT SUM(x), COUNT(*) FROM ...`) produces exactly one output row. This will be special-cased: a **single group is pre-created** with zero-initialized states and **no hash table is built or probed at all.** Every batch folds directly into that one group's states (the same per-aggregate `update`, with `gidx[i] ≡ 0`). This avoids paying any hashing, probing, or directory machinery for the degenerate one-group case, and it makes the global-aggregate path a clean, branch-free streaming fold.

The **finalize-on-empty** semantics this relies on are **(as-built)** correct in the kernels: `FinSum`/`FinMinMax` emit NULL when `has == false`, and `FinAvg` emits NULL when `count == 0`. So a global aggregate over **zero input rows** is *designed* to emit one row — `COUNT(*) = 0`, `SUM`/`MIN`/`MAX`/`AVG` = `NULL` — because the single group exists independently of whether any row arrived, and its finalize kernels already produce NULL on an untouched state. The *driver* that pre-creates that group and runs the drain is **not yet built**; the emitted-row behavior must be backed by a zero-row global-aggregate test before it is claimed as shipped.

### Aggregate state and the SUM/AVG/MIN/MAX/COUNT specifics (as-built kernels)

The state structs and their semantics below are **as-built** in `src/exec/aggregate.cpp`. The **overflow/precision behavior is stated precisely and honestly** — it is a deliberate simplification, not "correct production behavior."

- **`COUNT(*)`** — `CountSt { std::int64_t count }`, a **signed `int64`** incremented once per input row, unconditionally (counts rows, including those with NULL in other columns). Output type `int64`.
- **`COUNT(col)`** — same `int64` state, incremented only when `col` is valid (NULL inputs do not count). Output type `int64`.
- **`SUM`** — `SumSt<A> { A sum; bool has }`. As-built accumulator widths and **honest overflow policy**:
  - `SUM(int32)` accumulates into an **`int64`** accumulator (`A = int64`), output `int64`. This is a *partial* widening (4-byte input → 8-byte accumulator).
  - `SUM(int64)` accumulates into an **`int64`** accumulator (`A = int64`), output `int64`. **No widening** — there is no wider integer accumulator.
  - In **both** integer cases the add is `simd::scalar::AddOp<int64_t>`, which is **two's-complement wrapping** (UBSan-clean via unsigned-cast arithmetic, per ADR 0008). The `int64` accumulator therefore **can silently overflow and wrap** — e.g. a large dataset of int32s whose true sum exceeds `int64` range, or any `int64` sum that overflows. We do **not** claim DuckDB's behavior (DuckDB raises an overflow error or promotes the result type). This is the same documented divergence as ADR 0008's wrapping-arithmetic note.
  - `SUM(double)` accumulates into a `double`. See the FP-associativity note below.
- **`MIN` / `MAX`** — `MinMaxSt<T> { T value; bool has }`, a running extremum of the value type; `has=false` until the first valid input initializes it (an all-NULL group yields `NULL` via `!has`).
- **`AVG`** — `AvgSt<A> { A sum; std::int64_t count }`, divided **once at finalization** (`FinAvg`: `double(sum)/double(count)`), not maintained as a running mean. NULL inputs are excluded from both `sum` and `count`; a group with zero non-NULL inputs finalizes to `NULL` (the `count == 0` guard, **no divide-by-zero**). **Overflow caveat (as-built):** `AVG(int32)` and `AVG(int64)` use an **`int64` `sum` accumulated with the same wrapping `AddOp`** as `SUM` — so `AVG(int)` carries the **identical silent-overflow exposure** as `SUM(int)`. `AVG(double)` accumulates the sum in `double`.

**Floating-point non-associativity (honest, and missing from the draft):** `SUM(double)` and `AVG(double)` accumulate in **batch/probe order** via a scatter fold. IEEE-754 addition is **non-associative**, so the `double` result depends on input and batch ordering and is **not expected to be bit-identical to DuckDB** (which may accumulate in a different order, or use compensated summation). Any DuckDB comparison for floating-point aggregates must compare **within an epsilon**, not for exact equality. We make no bit-exact-FP claim.

All NULL-input handling is "skip the fold" driven by the input validity bitmask (ADR 0003): the kernels check `col->validity().RowIsValid(i)` per row. (The all-valid fast path of ADR 0003 lets a NULL-free input column take a branch-predictable cheap path; the as-built kernels currently call `RowIsValid` per row, which hits that fast path internally.)

**Supported `(function, input-type)` matrix (as-built — `ResolveAggregate`).** Unsupported combinations return a `ResolvedAggregate` with null function pointers; the `HashAggregate` constructor is to **assert** they are non-null at setup.

| Function | Supported input types | Output type |
|---|---|---|
| `COUNT(*)` | any (input ignored) | `int64` |
| `COUNT(col)` | any | `int64` |
| `SUM` | `int32`, `int64` → `int64`; `double` → `double` | as left |
| `MIN` / `MAX` | `int32`, `int64`, `double`, `date` | same as input |
| `AVG` | `int32`, `int64`, `double` | `double` |

Notably unsupported as-built: `SUM`/`AVG` on `date`, and `MIN`/`MAX`/`SUM` on `VARCHAR`. Those resolve to null fns and assert at setup rather than silently misbehaving.

### VARCHAR keys are copied into the table's own StringHeap (intended)

A VARCHAR group key arrives as a 16-byte `StringRef` whose long-string (`length > 12`) bytes live in the **input batch's** `StringHeap` (ADR 0004), which is freed when the borrowed batch's `consume()` returns (ADR 0006/0007). A retained key pointing into it would dangle. Therefore, **on inserting a new group**, a long-string key will be **copied into the hash table's own `StringHeap`** via `StringHeap::Add` (the bump-allocator arena of ADR 0004, which already does inline-vs-copy correctly — `include/strata/data/string_heap.hpp`), and the row will store a `StringRef` pointing into *that* heap. The table owns the bytes for the lifetime of the aggregation. Inlined keys (`length <= 12`) need no copy — they are self-contained in the 16 bytes. This copy happens **once per distinct string group at insertion**, never on a probe hit (a hit compares against the already-copied stored key), so the cost is bounded by group cardinality, not by row count.

**Status:** the `StringHeap::Add` API exists and is correct; the aggregation code does **not** call it yet (the as-built kernels handle numeric/date state only — there is no VARCHAR key path or row buffer). This is designed-not-built and must reference a real call site before it is claimed as shipped.

### Load factor, growth, and the linear-probe clustering tradeoff (intended)

- **Capacity will be a power of two**; slot selection is `hash & (capacity - 1)` (a mask, no modulo).
- **Target load factor below ~0.7.** When `groups / capacity` would exceed the threshold, the slots array **doubles** and every group is re-slotted by its stored hash (above). Open addressing degrades sharply as it fills — probe sequences lengthen super-linearly near 100% — so we grow *well* before full; 0.7 is the standard knee that trades a little memory for short, bounded probe sequences. (This is an **intended target**, not an implemented constant; there is no `kLoadFactor` in the tree yet. The figure will be tied to a named constant once the table exists.)
- **Linear probing clusters** (Knuth's primary clustering: occupied runs coalesce, so a collision near a cluster tends to lengthen it). We accept this knowingly: linear probing is **maximally cache-friendly** — the next probe step is the next slot, a sequential access that is very often in the same cache line — and the salt makes each step in a cluster a one-byte rejection rather than a key dereference. Sub-0.7 load keeps clusters short. We chose simplicity + cache behavior + salt over the lower clustering of quadratic / double hashing, whose non-sequential probe steps throw away the locality that is the whole point.

### Draining the table: groups may exceed `kVectorSize` → multiple output chunks (intended)

At end-of-input the sink will **drain** the row buffer into output `DataChunk`s: for each group, `finalize` the aggregate states **(as-built finalize kernels)** (e.g. `AVG = sum / count`, NULL-on-empty) and emit the key columns + aggregate results. There can be **more groups than `kVectorSize` (2048)** rows, so the drain naturally **spills across multiple output chunks** — it walks the group rows and packs up to 2048 finalized groups per output chunk, starting a new chunk when full (the same chunking discipline as the loader, ADR 0007). The output is unordered (hash order); any `ORDER BY` is a separate downstream operator.

## Alternatives Considered

- **`std::unordered_map<Key, State>` (or any node-based / separate-chaining map).** Rejected for the hot path. It is **node-based**: each group is a separately heap-allocated node, so (a) every distinct group is a heap allocation (vs. our one growable buffer + amortized doubling), (b) a probe **chases pointers** to nodes scattered across the heap — the opposite of the row layout's one-contiguous-region touch, and (c) there is **no salt**, so every chain step that reaches a node must compare the (possibly string) key, with no one-byte pre-rejection. The standard map also imposes reference/pointer stability guarantees we do not need and that *force* node allocation. For a structure probed billions of times in a TPC-H aggregation, the per-group allocation + pointer-chasing + salt-less compares are exactly the costs we are designing away.

- **Separate-chaining hash table (our own, array of bucket heads + linked nodes).** Same pointer-chasing and per-node allocation as above, even if the node pool is arena-backed: a chain walk dereferences a `next` pointer per step, and the key+state are not guaranteed contiguous. Open addressing with linear probing keeps probing sequential and the key+state co-located. Rejected.

- **Keys and aggregate states in two parallel arrays (struct-of-arrays), not one row.** Tempting for SIMD over states. Rejected for high-cardinality grouping: it splits a group across two cache-line regions, so a probe that confirms a key (touching the key array) and then updates state (touching the state array) pays **two** cache misses where the row layout pays one — and the high-cardinality update is latency-bound (gather), so SoA buys no SIMD win that would offset the extra miss (ADR 0008). The row layout's key+state adjacency is the deliberate choice. (We note the counterweight: for low-cardinality grouping, SoA over a handful of resident state lines *could* vectorize a reduction — see the qualified SIMD note above. We still prefer the row layout for its general-case locality and accept leaving the low-cardinality vectorization unexploited.)

- **No salt (group index only in the slot).** Rejected: without the salt, *every* non-empty slot encountered during a probe must dereference the row buffer to compare the key — a cache miss + a (possibly string) compare per step. The 1-byte salt rejects the overwhelming majority of non-matching slots from data already in the slot's own cache line, turning most probe steps into a byte compare with no row dereference. This is the single largest constant-factor win in the probe.

- **Re-hashing keys on growth (no stored hash).** Rejected: re-hashing every key on every doubling re-reads all key material — for string keys, the inline prefix and the `StringHeap` tail (ADR 0004) — repeatedly. Storing the `uint64` hash per row makes re-slotting read only the hash field; growth never re-hashes keys (it still re-inserts, see above). The 8 bytes/row is the price of avoiding the key re-hash, especially for string-keyed aggregations.

- **`int128` (`__int128`) accumulators for integer `SUM`/`AVG`.** Considered and **rejected for P4**, with the tradeoff stated plainly rather than implied away. A 128-bit accumulator would make integer `SUM`/`AVG` overflow practically impossible for TPC-H-scale data, closing the gap with DuckDB's promote-on-overflow behavior. But `__int128` is a **non-standard GNU/Clang extension**: it is not a standard C++ type, and using it trips `-Wpedantic` (ADR 0002 builds with `-Wpedantic -Werror`), so it would have to be locally `#pragma`-guarded or wrapped — added complexity and a portability seam across the clang-on-mac / gcc-on-ubuntu matrix. For P4 we accept the **`int64` accumulator with documented two's-complement wrapping** (above) as the simpler choice, and record the overflow exposure honestly instead of claiming it away. Promoting to `int128` (or to a checked/saturating accumulator) is a clean future change isolated to `SumSt`/`AvgSt` and the kernels.

- **Encoding NULL as a sentinel value inside the key bytes.** Rejected: there is no safe sentinel for a full-range `int64`/`double` column, and it conflates a real value with NULL. The explicit per-key null-flag byte is unambiguous, supports NULL == NULL grouping cleanly, and keeps the stored key self-contained.

- **Routing the global (no-`GROUP BY`) aggregate through the hash table with one synthetic key.** Rejected as needless: it would pay hashing, probing, a directory, and growth checks to maintain a structure that can only ever hold one group. The pre-created single group is a strictly simpler, faster, branch-free streaming fold.

- **A SIMD-vectorized scatter-update of aggregate states (general case).** Rejected for **high-cardinality** grouping on the ceiling, not on ambition (ADR 0008): the state updates are a data-dependent scatter indexed by `gidx[i]`, with no native NEON gather/scatter and a memory-latency bottleneck. Vectorizing the arithmetic does not help when the limiter is the scattered state loads. (For low-cardinality / global aggregation the calculus flips and a vectorized reduction is viable — we leave that unexploited for now, deliberately. See the qualified SIMD note.)

## Consequences

**Wins (intended, except where flagged as-built)**

- **One cache-line region per probe hit.** Key and aggregate state will be co-located in one row, so confirming a group and updating it touch the same hot region — no second structure, no pointer chase to a state node.
- **Most probe steps will be a one-byte rejection.** The salt rejects non-matching slots without dereferencing the row buffer; full key compares (and string compares) happen only on a salt match.
- **No per-group heap allocation.** Groups will live in one growable buffer, amortized-doubled; no `unordered_map`-style per-node `new`.
- **Cheaper growth (no key re-hash).** Doubling re-slots by the **stored hash** — no key re-hash (the expensive part for string keys). It still pays an O(groups) re-insertion into the new directory; the saving is the re-hash, not the re-insertion.
- **(as-built) Correct NULL-on-empty finalize.** `FinSum`/`FinMinMax`/`FinAvg` already emit NULL for an all-NULL or zero-input group; the global zero-row case and all-NULL groups are correct *at the finalize layer* today.
- **Intended grouping NULL semantics.** Per-key null-flag is designed to give NULL-as-its-own-group (NULL == NULL for grouping, distinct from non-NULL), including multi-key combinations. To be confirmed by a DuckDB-diff test (not yet present).
- **(as-built) Per-batch dispatch amortization.** The state kernels already loop internally per batch, no per-row dispatch — consistent with ADR 0008/0009.
- **Fast degenerate path (intended).** Global aggregation will skip the hash table entirely — a single pre-created group folded in a branch-free pass.
- **Self-contained retained string keys (intended).** VARCHAR keys to be copied into the table's own `StringHeap` once at insertion via the existing `StringHeap::Add`.

**Tradeoffs (stated honestly)**

- **The high-cardinality update is a random-access scatter — not a SIMD target.** It is probe-/memory-latency bound, not lane-bound (ADR 0008). We get no SIMD win on that fold and do not claim one. **Low-cardinality and global aggregation are not latency-bound** and could be vectorized; the as-built kernels currently run scalar there too, leaving that win unexploited by choice.
- **Integer `SUM`/`AVG` wrap silently (as-built).** `int64` accumulators with two's-complement wrapping (`AddOp`); `SUM(int64)` has **no** wider accumulator. We do **not** match DuckDB's overflow-error/promotion behavior. `int128` was considered and rejected for P4 (GNU-extension / `-Wpedantic` cost) — documented above and in ADR 0008's wrapping note.
- **Floating-point `SUM`/`AVG` are order-dependent (as-built).** IEEE-754 non-associativity over batch-order accumulation means `double` results are **not bit-identical to DuckDB**; comparisons must be within epsilon. No bit-exact-FP claim.
- **Open addressing degrades near full.** Probe sequences lengthen super-linearly as load → 1. To be mitigated by growing below ~0.7 load, at the cost of memory (slots array mostly empty by design) and an O(groups) re-insertion on each doubling.
- **Linear probing clusters** (primary clustering). Accepted for its sequential, cache-friendly probe steps and because the salt makes each cluster step a one-byte reject; sub-0.7 load keeps clusters short.
- **String key hashing and comparison are scalar.** German-string equality short-circuits on length + 4-byte prefix and only `memcmp`s the tail on a match (ADR 0004), but it does not vectorize per element; string-keyed `GROUP BY` is correspondingly scalar on the compare path. The salt still pre-rejects most non-matches before any string work.
- **8 bytes/row for the stored hash.** Real memory cost, paid to avoid key re-hash on growth; a deliberate space-for-time trade that pays off most for string keys.
- **Result spills across output chunks.** More than `kVectorSize` groups means multiple output `DataChunk`s; output is in hash order (unordered), so any required ordering is a separate operator downstream.
- **Restricted type matrix (as-built).** `SUM`/`AVG` on `date` and `MIN`/`MAX`/`SUM` on `VARCHAR` are unsupported and assert at setup (see the matrix above).
- **Not yet built / not yet validated.** The hash table, probe, salt, growth, StringHeap copy, and global-aggregate driver are designed-not-built; there is no aggregation or DuckDB-diff test. Status stays *Proposed* until they exist.

## How to defend this at a whiteboard

- **First, what's actually built?** Be upfront: P4 ships the **per-group state kernels** (`update`/`finalize` function pointers for COUNT/SUM/MIN/MAX/AVG, with zero-init = correct initial state). The **hash table** — slots, salt, stored hash, row buffer, growth, StringHeap copy, probe loop, global driver — is the **designed** structure, not yet coded. I won't claim a probe loop I haven't written.
- **Why not `std::unordered_map`?** It's node-based: one heap allocation per group, a pointer chase per probe to a scattered node, and no salt so every reached node compares the full (maybe string) key. The design is **open addressing + linear probing + a row layout**: key and state in one contiguous row, one growable buffer (no per-group alloc), and a salt so most probe steps reject on one byte without touching the row.
- **What's in a row?** `[ uint64 hash | packed key values | per-key null-flag byte | packed aggregate states ]`. Key and state adjacent so a probe hit touches one cache-line region. The slots array is separate and tiny: `(group index or empty-sentinel, salt)`.
- **Walk a probe.** Hash the key; start at `hash & (cap-1)`; linear-probe. Empty slot ⇒ new group, append a row (store hash, keys, null-flags; states stay zero), write `(index, salt)`. Salt mismatch ⇒ next slot, **no row dereference**. Salt match ⇒ dereference the row, full key compare; equal ⇒ found, unequal (collision) ⇒ keep probing.
- **Why the salt?** Top hash bits cached in the slot. Without it every non-empty slot you walk past costs a row dereference + key compare. With it, the common rejection is a one-byte compare on data already in cache. Biggest constant-factor win in the probe.
- **Why store the hash in the row?** Growth (double the slots, re-place every group) re-slots by the **stored hash** — `hash & new_mask` only, no key re-hash. That matters for string keys (rehash = touch prefix + chase into `StringHeap`). But be honest: growth still does an O(groups) re-insertion into the new (empty, short-probe) table — the saving is the re-hash, not the re-insertion. 8 bytes/row buys key-free growth.
- **NULL as a group?** Per-key **null-flag byte** in the row. NULL == NULL for *grouping* (all NULLs are one group — different from 3VL `NULL = NULL` being unknown), NULL ≠ any non-NULL, and each key's null-ness participates so `(NULL,'A')`, `('x',NULL)`, `(NULL,NULL)` are distinct. Intended semantics — I'd confirm it with a DuckDB diff test, which I haven't written yet.
- **Global aggregate (no GROUP BY)?** Designed special case: one pre-created group, **no hashing or probing**, just a streaming fold. Zero-row case emits one row (`COUNT=0`, others `NULL`) — and that's backed by the finalize kernels, which already return NULL on an untouched state (`!has` / `count==0`).
- **How does the update work, and is it SIMD?** Probe the whole batch once to get `gidx[]`, then **one `update` per aggregate** folds values into `state_of(gidx[i])` = `rows + gidx[i]*row_width + state_offset`. For **high cardinality** that's a **random-access scatter** — memory-latency bound, no native NEON gather — so **not a SIMD target**, and I don't claim a speedup. For **low cardinality / global** (few resident state lines) it's *not* latency-bound and a vectorized reduction is viable; I currently leave that on the table. The general win is dispatch amortization + row-layout locality.
- **COUNT type?** `int64` (signed, `CountSt`), output `kInt64` — not `uint64`. `COUNT(*)` counts rows; `COUNT(col)` counts non-NULLs.
- **SUM/AVG overflow — the honest answer.** `SUM(int32)`→`int64`, `SUM(int64)`→`int64` (no wider accumulator); both add with **wrapping** `AddOp`, so they **can silently overflow**. `AVG(int)` shares that wrapping `int64` sum — same exposure. DuckDB errors or promotes; I wrap and document it (ADR 0008). `int128` would fix it but it's a GNU extension that trips `-Wpedantic -Werror`, so I rejected it for P4 and isolated the change to the state structs for later.
- **Floating-point determinism?** `SUM(double)`/`AVG(double)` accumulate in batch order; IEEE-754 add is non-associative, so results are **order-dependent and not bit-identical to DuckDB** — compare within epsilon, not exactly.
- **AVG mechanics?** Keep `(sum, count)`, divide once at finalize; NULL inputs excluded from both; zero non-NULL inputs ⇒ `NULL`, no divide-by-zero.
- **String keys?** To be copied into the table's **own `StringHeap`** on insert (via the existing `StringHeap::Add`) so the stored key outlives the borrowed batch (ADR 0004/0006). Copy once per distinct string group, never on a hit. Compare short-circuits on length+prefix; salt pre-rejects most non-matches first. (Designed; the agg code doesn't call `Add` yet.)
- **What types are supported?** `COUNT(*)`/`COUNT(col)`→int64 (any input); `SUM`/`AVG` on int32/int64/double; `MIN`/`MAX` on int32/int64/double/date. `SUM`/`AVG` on date and `MIN`/`MAX`/`SUM` on VARCHAR are unsupported and assert at setup.
- **Load factor / probing tradeoff?** Intended: power-of-two capacity, mask not modulo; target load < ~0.7 and double when crossing (no constant in the tree yet). Open addressing degrades near full → grow early. Linear probing clusters (primary clustering) but its probe steps are sequential and cache-friendly, and the salt makes each step a byte reject — trade a little clustering for a lot of locality.
- **Output bigger than a vector?** More than 2048 groups ⇒ drain into multiple output chunks, hash order (unordered); ordering is a downstream operator.

---

**Implementation note (to reconcile before promotion to Accepted):** the header `include/strata/exec/aggregate.hpp` currently points to "ADR 0011" for the state/row layout; this is ADR **0010** and no 0011 exists. Update the header comment to reference ADR 0010 (or renumber consistently with `docs/adr/README.md`, which indexes this as 0010). Also: this ADR's index row in `docs/adr/README.md` must show **Proposed**, not Accepted, until the table + tests land.
