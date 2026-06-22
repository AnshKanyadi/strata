# 0012. Hash join: build/probe, chained build table, row layout, and vectorized match gather

## Status

Accepted. As-built for P5 (single-threaded INNER equi-hash-join). P8 parallelism is foreshadowed but **not** built.

## Context

P5 introduces the join operator. Strata needs to evaluate equi-joins (`a.k = b.k`, optionally composite `a.k1 = b.k1 AND a.k2 = b.k2`) over the columnar, push-based pipeline established in ADRs 0006–0009. TPC-H is join-heavy (the query set is fundamentally about joining `lineitem`/`orders`/`customer`/`part`/`supplier`), so this operator is on the critical path for the DuckDB comparison.

The canonical answer for an equi-join in an analytical engine is the in-memory **build/probe hash join** (Grace/classic hash join lineage): materialize one input into a hash table, then stream the other input against it. Sort-merge join is the main alternative and is rejected below. For P5 we scope to **INNER** join only.

Two structural facts drive the design:

1. **A join hash table retains every build row; the aggregate hash table collapses to one row per group.** ADR 0010 chose **open addressing** for the aggregate HT because aggregation folds many input rows into exactly **one persistent, mutable state row per unique group** — the slot *is* the group's identity, so a linear-probe array with one row per group is the natural fit. A join HT has no such collapse: **duplicate join keys are normal and expected** (a foreign key appears many times; a join key need not be unique on the build side either), and **every build row must be retained** so it can be emitted on a match. That difference — collapse-to-one-state-per-key vs retain-all-rows-including-duplicates — is what makes chaining the natural fit here (see Decision). Note this is a design *preference*, not a forced law: the reverse pairings are both buildable (an aggregate HT can be chained; a join HT can be open-addressed). We reject the reverse pairing for the join for the concrete reasons in *Alternatives Considered*.

2. **The build side is a pipeline breaker, and input chunks are borrowed (ADR 0006).** A Sink cannot retain pointers into the DataChunks it is fed — those buffers are owned upstream and reused after `Consume()` returns. The build table must therefore be **fully materialized** (deep-copied) before any probe row is processed. This makes a row-major copy not just convenient but mandatory, and it lets us choose the *layout* of that copy to optimize the probe.

## Decision

### Sides: build the smaller (goal/heuristic), probe with the larger

The intent is to build the **smaller** input into the hash table and **probe** with the larger, for two reasons:

- **Memory footprint:** hash-table memory scales with the build side, so building the smaller side minimizes resident memory and minimizes the build phase's deep-copy/allocation cost.
- **Cache residency (the lever that actually matters here):** a smaller build table is more likely to stay resident in L2/L3. Since probe-time build-row reads are random (chain walk), the dominant probe cost is *cache misses on that random access*. A more cache-resident build table directly reduces that miss cost — this, not just RAM footprint, is the primary performance reason to build the smaller side.

**Honest as-built caveat:** P5 has **no cardinality estimation**. The planner *statically designates* which physical input is the build side and which is the probe side; the operator is agnostic to which is which. "Build the smaller" is therefore the **goal/heuristic baked into our test plans**, not a property the operator dynamically guarantees. Cost-based side selection is deferred.

### Collision strategy: CHAINING

The join HT uses **separate chaining**, implemented without per-node heap allocation:

- A **slots array** of `uint32` indices, sized to a power of two, maps `slot = hash & mask` to the **head build-row index** of that slot's chain.
- Each materialized build row carries a **`uint32 next`** field pointing at the *previous* row inserted into the same slot.
- **Insert PREPENDS:** new row's `next` = current head; slot head = new row. O(1) insert, no resize of chain storage, no allocator traffic per row.
- **Index space / sentinel:** indices are `uint32`, so the build side is capped at **fewer than 2^32 rows**. The all-ones value **`0xFFFFFFFF`** is reserved as the **empty-slot and end-of-chain sentinel** (the slots array is initialized to it; a chain terminates when a row's `next` equals it). A build side beyond this cap would require widening to `uint64` indices or partitioning (P8).

The chain pointers live **inside the row buffer**, so the entire build table is two contiguous allocations (the slots array + the row buffer) with no scattered node objects.

### Build-row layout: ROW-MAJOR, contiguous

Each build row is packed as:

```
[ uint64 hash | build column values (packed) | build null-flags (1 byte/col) | uint32 next ]
```

Rationale: at probe time a confirmed match must emit the build side's payload columns. If the build side were stored *columnar*, emitting one matched row would touch one scattered location per build column. Storing all of a build row's columns **contiguously** means a match reads essentially one cache-line region. Since probe-time access to build rows is already random (chain walk — see below), keeping each row's bytes together is the one locality lever we have.

Materialization details:
- The build side is fully **deep-copied** into the row buffer during build (pipeline-breaker requirement).
- For VARCHAR columns (German strings, ADR 0004): the **16-byte string view is copied inline** with the row's packed column values **always**. Only the **out-of-line bytes of long strings (>12 bytes)** are relocated into the table's **own `StringHeap`**, so those spilled bytes outlive the borrowed input chunks. **Short strings (<=12 bytes) are stored inline in the view and need no heap copy.**
- **Build rows whose join key contains ANY NULL are EXCLUDED** from the table. A NULL key can never equi-match anything, so it would only waste a slot and a chain hop. Consequently, **stored build keys are never NULL** — the per-key NULL flags in the row exist for *payload* columns, not for proving key non-nullity.

### Probe

For each probe row in the incoming chunk:

1. **NULL key short-circuit:** if ANY probe join key is NULL, the row is **skipped** — no match emitted. (Equi-join NULL semantics; see below.)
2. Otherwise compute the probe hash (composite keys mixed identically to the build side — see Composite keys) and walk the chain at `slot = hash & mask`.
3. For each candidate build row on the chain: **quick-reject by comparing the stored 64-bit hash first.** Only if the hashes are equal do we perform the **full per-key value compare**. Because neither side's keys can be NULL by construction (build NULLs excluded, probe NULLs skipped), **the key comparison contains no NULL/3VL logic** — it is a plain value equality.
4. Each surviving candidate yields a **`(probe_row_index, build_row_index)` match pair**.

**On the quick-reject (precise framing).** The slot index already consumed the low `log2(capacity)` bits of the hash, so **candidates on the same chain already agree on those low bits**. The quick-reject's discriminating power therefore comes from the **remaining (high) bits** of the stored hash, not the full 64. It is still an effective filter — one integer compare rejects most non-matching same-chain candidates before we touch the (wider, possibly multi-column, possibly VARCHAR) key bytes — but "rejects almost everything" is only as true as those high bits are well-distributed. We store the **full `uint64`** (rather than the conventional 1–2 byte *salt*/tag) because we already compute it for slot placement and reuse it to recompute slots on any rehash; the cost is an explicit, accepted **8 bytes per build row**. A salt would save that space at the cost of recomputing or reloading more on rehash; we chose the simpler full-word store. (See *Alternatives Considered*.)

### Fan-out, buffering, and the vectorized gather

A single probe row can match **many** build rows (duplicate build keys), so the number of match pairs can **exceed** the probe input batch size. We therefore decouple matching from emission:

- Match pairs accumulate into a buffer. When the buffer reaches **`kVectorSize`**, we **FLUSH** by producing one output DataChunk via a **vectorized gather**:
  - **Probe output columns** are gathered by a selection vector of `probe_row_index` over the **still-live, borrowed** probe chunk.
  - **Build output columns** are gathered from the matched build rows (`build_row_index`) in the row buffer.
- The output schema is **probe columns followed by build columns**.
- Critically, the flush happens **inside the probe `Consume()` while the probe chunk is still alive** — respecting the ADR 0006 borrow rule. We never stash a pointer to the probe chunk to flush later; remaining buffered matches are flushed before `Consume()` returns (a final partial flush covers the tail).

**This batched gather of matched build rows IS what we mean by "vectorized" here.** It is *not* lane-parallel SIMD arithmetic. Two distinct memory-bound patterns are involved, and they are bound for *different* reasons:

- The **chain walk** is **dependent-load pointer chasing**: each `next` index must be loaded before the next load can even be issued, so the chain is traversed serially and is **latency-bound** per chain.
- The **match gather**, by contrast, is a batch of **independent random loads**: every `build_row_index` (and `probe_row_index`) is known up front when the buffer flushes. Independent loads are exactly what the out-of-order engine and hardware prefetcher overlap (memory-level parallelism), and exactly what a hardware gather instruction (e.g., AVX-512 `VPGATHER`) targets. It is random-access and memory-bound, but **not** dependent pointer-chasing.

Both stay **scalar** under ADR 0008's "SIMD ceiling" — the chain walk because serial dependent loads gain nothing from lanes, the gather because gathered loads are memory-bound rather than lane-bound — but the mechanisms differ. We are explicit about this so no one reads "vectorized hash join" as a SIMD-arithmetic claim. The win from batching is amortized per-call overhead, branch-prediction-friendly tight loops, and overlap of the gather's independent loads — not vector lanes.

### Composite (multi-key) joins

For multi-column keys, the per-key hashes are combined with the **same mix function applied identically on both build and probe sides**. Equal composite keys therefore produce equal combined hashes and land in the same slot; the chain-walk value compare then checks every key column for equality. There is no canonical-ordering subtlety because both sides hash the key columns in the same order with the same combiner.

## Alternatives Considered

- **Open addressing (linear probing) for the join HT.** Rejected for *this* engine — but on accurate grounds. Open addressing is **fully capable of handling duplicate join keys**: you keep one probe-table slot per **build row**, and duplicates simply occupy adjacent probe slots. This is a real, production technique (DuckDB, for instance, uses radix-partitioned linear-probing-style pointer tables for joins), and it does **not** "blow up the table" — a chained design also stores exactly one record per build row (every row sits in the row buffer with a `next` field) plus a slots array, so the row counts are identical. We reject it here because chaining better fits our materialized, duplicate-heavy build table: **(a)** inserts are O(1) prepend with **no load-factor-driven rehash of the already-materialized payload** (linear probing must keep load factor below ~1 and rehash/relocate rows as it fills); **(b)** **no tombstones and no primary clustering** pathology; and **(c)** duplicate keys form an **explicit per-key list** that a chain walk traverses directly, whereas linear probing **interleaves distinct keys' rows** in a run and must re-compare keys across that run. We concede the genuine counter-tradeoff: **linear probing can have better probe-time cache behavior** (contiguous scan of a run vs. pointer-chasing a chain). For P5's build-once/probe-streaming, allocation-free build and direct per-key lists won.
- **Columnar build-side storage.** Rejected for the build table: emitting a matched row would scatter one load per column. Row-major keeps a matched row's bytes in one region, which matters because the access pattern is already random. (Note the asymmetry: the *probe* side stays columnar — we only gather the columns we output, by selection vector, over a live chunk.)
- **A small salt/tag instead of the full stored hash.** Considered and not adopted for P5. A 1–2 byte salt (the conventional choice) saves 7–8 bytes per build row. We store the full `uint64` because it is already computed for slot placement and is reused to recompute slots on rehash; the per-row space cost is accepted and documented. A salt remains an easy future space optimization.
- **Sort-merge join.** Rejected for P5: requires sorting (or pre-sorted/partitioned inputs) of both sides. For unsorted analytical inputs **and reasonable key distributions**, hash join's expected-linear build + probe beats `O(n log n)` sorting, and hash join needs no global order. Sort-merge wins mainly when inputs are already ordered, when interesting orders can be exploited downstream, or — relevant to our own skew tradeoff below — when worst-case predictability matters: under severe build-side key skew, hash join's probe degrades toward the long-walk case noted in *Consequences*, at which point sort-merge's `O(n log n)` worst case can be more predictable. Neither condition is our P5 case.
- **Nested-loop / block-nested-loop join.** Rejected: quadratic; only justifiable for non-equi predicates, which P5 does not target.
- **Per-node heap-allocated chain nodes.** Rejected: allocator traffic per build row and pointer-chasing across scattered allocations. Embedding `next` as a row-buffer index keeps the table in two contiguous blocks and makes indices trivially relocatable/serializable (which also helps the P8 merge path).
- **Three-valued (WHERE-style) NULL handling inside the key compare.** Rejected as both wrong and wasteful: an equi-join does not return UNKNOWN rows — a NULL join key simply never matches. Pushing the NULL decision to the *edges* (exclude NULL build keys, skip NULL probe keys) means the inner compare is pure value equality with zero NULL branches on the hot path.

## Consequences

**Wins**
- One operator covers single-key and composite INNER equi-joins.
- O(1), allocation-free build inserts; the whole table is two contiguous allocations.
- Probe match emission is a clean batched gather producing standard `kVectorSize` chunks that feed the rest of the push pipeline unchanged.
- The stored-hash quick-reject makes most chain hops a single integer compare (discriminating on the high bits not consumed by the slot index).
- NULL handling lives entirely at the boundaries, so the inner compare is branch-light value equality.
- Row-major build layout gives the best available locality for an inherently random build-row read.

**Tradeoffs (stated honestly)**
- **Probe chain-walk is dependent-load pointer chasing — memory-latency bound, serial per chain.** This is the dominant cost and a reason it is not a SIMD win. (The match gather is also memory-bound but is MLP-friendly independent loads, not dependent chasing.)
- **The entire smaller side is resident in memory.** No spilling/partitioning to disk; a build side larger than memory is not handled (P5 assumes it fits).
- **`uint32` index space caps the build side at <2^32 rows**, with `0xFFFFFFFF` reserved as the empty/end-of-chain sentinel. Beyond that requires `uint64` indices or partitioning.
- **The row layout duplicates the key bytes:** the key is hashed into the leading `uint64` *and* stored again inside the packed column values (we must keep the real key for the exact compare), plus 8 bytes for the stored hash. This trades space for a self-contained row.
- **INNER only.** LEFT-OUTER is deferred: it needs a per-probe-row "matched" flag and an emit-build-columns-as-NULL path for unmatched probe rows (and FULL/RIGHT additionally need a build-side matched bitmap and a post-probe scan for unmatched build rows). None of that is built in P5.
- **No hash-table partitioning / radix partitioning yet** (deferred to P8). The single shared table means no cache-partitioning benefit and no natural parallel build boundary today.
- **Skew degrades to a long walk.** One key value with a huge chain (a heavily duplicated build key) turns probes of that value into a long linear scan; expected-linear probe holds only under reasonable (uniform-ish, bounded-chain) key distributions. There is no per-chain bound or skew mitigation in P5.

## Foreshadow: P8 parallelism (intended, NOT built, NOT yet benchmarked)

The build/probe shape is friendly to morsel-driven parallelism (Leis et al., *"Morsel-Driven Parallelism: A NUMA-Aware Query Evaluation Framework for the Many-Core Age"*, SIGMOD 2014):

- **Build parallelizes well — most cleanly via thread-local partitions then merge.** Workers build **thread-local row buffers/partitions** that are merged afterward (concatenate row buffers, then rebuild/relink the slots array). This is cheap precisely because `next` is a **relocatable index, not a raw pointer**. A **shared-table** variant is also possible — workers atomically prepend onto slot heads — but it is **not "naturally" parallel**: each prepend is a compare-and-swap on the slot head plus a write of the new row's `next`, i.e. the classic lock-free-linked-list CAS-retry loop, with the usual ABA hazard if indices are ever recycled. It is a real synchronization cost and correctness concern, and not obviously a win over partition-then-merge.
- **The INNER probe READ path is lock-free**, but probe parallelism is not free of coordination. Morsel-driven execution still requires **(a)** a shared atomic **morsel dispatcher/cursor** (the dispatcher is central to Leis 2014), **(b)** per-worker **output buffers** and a result-merge/ordering story, and — for the foreshadowed **outer joins** — **(c)** a **synchronized build-side matched bitmap** (atomic OR, or per-thread bitmaps merged at the end). So the read path **scales well**, but "no synchronization / embarrassingly parallel" would overstate it.

This is the intended P8 direction only; P5 ships the single-threaded operator described above, and none of the parallel variants have been built or benchmarked.

## How to defend this at a whiteboard

- **"Why chaining here when your aggregate HT is open-addressed?"** Not a cardinality law — both structures can be built either way. The real driver: aggregation collapses many rows into **one mutable state row per unique group**, so a slot-per-group open-addressing table is the natural identity; a join must **retain every build row including duplicates**, which maps cleanly onto a per-slot chain. Open addressing *can* do joins (DuckDB uses linear-probing pointer tables); we prefer chaining for allocation-free O(1) prepend with no payload rehash, no tombstones/clustering, and a direct per-key list — conceding linear probing can have better probe-time cache behavior.
- **"How is the chain stored without allocating nodes?"** `next` is a `uint32` index living inside each row in one contiguous row buffer; the slots array holds head indices; insert prepends. Two allocations total. `0xFFFFFFFF` is the empty/end-of-chain sentinel; build is capped at <2^32 rows.
- **"Why row-major, not columnar, for the build side?"** Probe-time build-row reads are random; row-major keeps a matched row's bytes in one cache-line region instead of scattering one load per column. The probe side stays columnar — we only gather the output columns we need.
- **"Why must the build side be fully materialized?"** It's a pipeline breaker and input chunks are *borrowed* (ADR 0006) — we can't keep pointers into upstream buffers, so we deep-copy. The 16-byte German-string view copies inline with the row; only long strings' (>12 byte) out-of-line bytes go to our own StringHeap. Short strings need no heap copy.
- **"How do you handle NULLs?"** Not with 3VL. Exclude build rows with NULL keys (can't match), skip probe rows with NULL keys (can't match). The inner key compare then has zero NULL logic — pure equality. This is the equi-join NULL rule, *not* WHERE-clause UNKNOWN semantics.
- **"What's the quick-reject, and why the full hash not a salt?"** Compare the stored hash before touching key bytes. Same-chain candidates already share the low `log2(capacity)` bits (those placed them in the slot), so the reject power is in the *high* bits — still effective. We store the full `uint64` (8 bytes/row, accepted) because we reuse it on rehash; a 1–2 byte salt is the conventional space-saving alternative and a fair future optimization.
- **"One probe row matches many build rows — how do you not blow the batch?"** Fan-out is buffered; at `kVectorSize` matches we flush a chunk via a vectorized gather (probe columns by selection vector over the live chunk, build columns from matched rows), output = probe cols then build cols, flushed while the probe chunk is still alive.
- **"Is this 'vectorized join' a SIMD win?"** No. The vectorized part is the *batched gather*. The chain walk is dependent-load pointer chasing (serial, latency-bound); the gather is independent random loads (MLP-friendly, what hardware gather targets). Both stay scalar per ADR 0008's ceiling, for different reasons. SIMD lanes don't help either.
- **"Which side do you build, and why does it matter?"** The smaller — primarily so the table stays **cache-resident** (cutting the random-probe miss cost that dominates), and secondarily for RAM footprint. As-built honesty: P5's planner statically designates the build side; "smaller" is a heuristic baked into our plans, not dynamic cardinality-based selection.
- **"How would you parallelize it?"** Morsel-driven (Leis 2014). Build: thread-local partitions then merge (cheap because `next` is an index) — or a shared table with atomic CAS prepend, which carries lock-free-list/ABA cost and isn't obviously better. Probe: the INNER read path is lock-free, but you still need a shared morsel dispatcher and per-worker output handling; outer joins add a synchronized matched bitmap. Intended for P8, not built.
- **"Where does it hurt?"** Dependent-load pointer-chasing probe (latency-bound), whole smaller side must fit in RAM, key bytes + 8-byte hash duplicated per row, `uint32` row cap (<2^32), INNER-only (no outer yet), no radix partitioning yet, and skew (one giant chain) degrades the probe toward a long linear walk — so "expected linear" holds only under reasonable key distribution.
