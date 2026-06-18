# 0006. Push-based execution scaffolding: Source / Sink / Pipeline

## Status

Accepted — and **realized as-built in P2**: `Scan`, `Pipeline`, `Sink`, and `ResultCollector` now exist with exactly the contract below, and 60 tests pass under ASan/UBSan and TSan. This record was drafted as a forward design just before P2 landed (in the precedent of ADRs 0004/0005, which spec ahead of their phase), so parts of the body keep the design ("will" / "is designed to") voice — read them as "now does." One naming difference from the shipped code: the source's type accessor is `output_types()` returning `std::span<const TypeId>` (not the illustrative `GetTypes()`/`const std::vector<LogicalType>&` shown below), and Strata's fixed type set is `TypeId`, not `LogicalType`.

## Context

ADR 0001 committed Strata to **push-based execution**: sources push `DataChunk`s into sinks, and the *executor* — not the operators — holds the driving loop. That ADR settled the control-flow *direction* and sketched interface shapes (`GetData` on the source, `OperatorResult Execute(DataChunk& input, DataChunk& output)` for streaming operators, `void Sink(DataChunk& input)` for breakers, with the loop `while (auto chunk = source->GetData()) { sink_chain->Consume(chunk); }`). It did not pin down the concrete C++ contract for the *endpoints* of a pipeline. P2 needs that contract so a `Scan` over a `ColumnarTable` can feed a `ResultCollector` and produce output verifiable against DuckDB.

Three roles must be named and given lifetimes:

- **Source** — produces batches of `VECTOR_SIZE`-bounded rows (a `DataChunk`).
- **Sink** — consumes batches; sinks are the *pipeline breakers* (later: hash aggregate, join build, sort buffer; in P2: a `ResultCollector`).
- **Pipeline** — the executor object that owns the loop wiring one source to one sink.

P2 will deliberately ship only the two endpoints (`Scan`, `ResultCollector`) with nothing in between. Streaming transform operators (Filter/Project) are a P3 concern and are only forward-referenced here; they are **not built**, and neither is anything in this ADR.

The central design question this ADR answers: **how does a batch cross the Source→Sink boundary?** The choice is between the executor handing the source a buffer to fill (copy-in) versus the source handing back a pointer it owns (borrow). We choose the borrow, and the rest of this ADR is the contract and consequences of that choice.

## Decision

### Source

```cpp
const DataChunk* GetChunk();          // next non-empty batch, or nullptr when exhausted
std::span<const TypeId> output_types() const;        // output column types (as-built)
```

`GetChunk()` will return the next **non-empty** batch, or `nullptr` to signal exhaustion. The non-empty guarantee is a **Source-side obligation**: a `Scan` whose underlying storage contains empty internal units (e.g. an empty row group) must loop past them internally — one `while` inside `GetChunk` — so that a returned pointer always denotes a chunk with `size() > 0`. This keeps the `Pipeline` loop free of an empty-chunk check: the loop keys off `nullptr` alone. (The *partial final chunk*, `0 < size() < 2048`, is a normal non-empty batch and is returned like any other; only genuinely empty units are skipped.)

The returned pointer will be **borrowed, not transferred**: the chunk is owned by (or owned-through) the source and is valid **only until the next call to `GetChunk()`** on that source. There is no ownership transfer and no per-batch chunk copy across the boundary.

This is what is **designed to make `Scan` zero-copy**: `Scan` will return a pointer **straight into the `ColumnarTable`'s stored chunks** — no allocation, no memcpy, no string materialization on the boundary. The batch the sink sees *is* the table's resident data.

### Sink

```cpp
void Consume(const DataChunk& chunk);  // process one borrowed batch
void Finalize();                        // single-thread completion; see P8 note below
```

`Consume` will take the chunk by `const&`. A sink that needs to **retain** data past the current call must copy it out before returning, because the borrow expires on the next `GetChunk()` (see lifetime contract below). `Finalize()` runs after the source is exhausted; it is where a breaking sink completes its work (build the hash table, sort the buffer, finish the result set). In the single-threaded P2 case `Finalize()` is called **exactly once**; that "exactly once" holds only in the one-worker degenerate case, and the parallel sink contract is a P8 extension (see the morsel section — under N workers the sink needs a per-worker local finalize/`Combine` plus a single global merge, which a single `Finalize()` does not by itself express).

### Pipeline

The pipeline *is* the loop. This **realizes ADR 0001's control-flow direction** (it is not a literal transcription — ADR 0001's `GetData`/`Execute`/`Sink` sketch is concretized here under different names, reconciled below):

```cpp
void Pipeline::Run() {
    const DataChunk* c;
    while ((c = source_.GetChunk()) != nullptr)
        sink_.Consume(*c);
    sink_.Finalize();
}
```

The executor holds the `while`; the operators hold no control flow. Data flows **forward**, source → sink. In P2 `source_` will be a `Scan` and `sink_` a `ResultCollector`. In P3 a chain of streaming transforms will sit between them, each owning its own output chunk and pushing it onward.

**Reconciliation with ADR 0001's interface sketch.** ADR 0001 named the source method `GetData` and the breaker `void Sink(DataChunk&)`; this ADR uses `GetChunk` and splits the breaker into a per-batch `Consume(const DataChunk&)` plus a one-shot `Finalize()`. The split is the substantive refinement: a single `Sink(chunk)` call conflates "absorb this batch" with "you are now complete," but a pipeline breaker needs those as two distinct events — absorb happens N times (once per pushed chunk), completion happens once (after the source drains). Separating them is also what makes the P8 parallel contract expressible (per-worker `Consume` streams, a later global merge in/after finalize). ADR 0001's streaming-operator signature `OperatorResult Execute(DataChunk& input, DataChunk& output)` is **not** instantiated here at all, because P2 ships no streaming operators; it remains the P3 contract.

### The borrow-lifetime contract (stated explicitly)

> A `const DataChunk*` returned by `GetChunk()` is valid only until the next `GetChunk()` call on the same source. It is a borrow into source-owned storage. A sink that outlives the borrow **must deep-copy** what it keeps.

`ResultCollector` will honor this: it deep-copies every consumed chunk into its own accumulated storage, **including copying VARCHAR payloads into its own `StringHeap`**. The reason is **ownership/lifetime, not relocation**: under the zero-copy scan a borrowed long `StringRef` is a bare `const char*` pointing into the **source's** `StringHeap` (the table's heap). Per ADR 0004 / `string_heap.hpp`, committed bytes in a `StringHeap` **never move** for the heap's lifetime — so the pointer does not dangle *while that heap lives*. But the collector must own its data **independently of the table**, and it cannot assume the source's heap will outlive the collector; if it kept the borrowed `StringRef` it would alias storage it does not own, which dangles whenever that source heap is released. So the collector copies the bytes into its own `StringHeap` (via `StringHeap::Add`) and rewrites the `StringRef` to point there. Short strings (≤ 12 bytes, inline) carry their bytes in the 16-byte POD itself and need no heap copy; only the long-string arm touches the source heap.

### Why `const`

The input is `const` because no downstream operator mutates its input:

- A **Filter** (P3) narrows its input via a *selection vector* — it does not overwrite input values.
- A **Project** (P3) writes a *fresh* output chunk — it reads inputs and produces new columns.

So `const DataChunk&`/`const DataChunk*` is the honest type: inputs are read-only views; transforms produce new chunks they own.

One plumbing point this makes explicit, because it bears on the `const` justification: the **as-built `DataChunk` has no selection-vector field** — `include/strata/data/data_chunk.hpp` carries only `columns_` and `size_`. A P3 Filter therefore *cannot* attach its narrowing to the borrowed `const` chunk (there is no slot to hang it on, and the chunk is read-only anyway). The Filter must emit its `SelectionVector` **out-of-band** (alongside the chunk in the pipeline's per-operator state) or produce a fresh output chunk. Whether the selection rides beside the chunk or a new chunk is materialized is a P3 design point; either way it confirms rather than strains the `const`-input contract — the input is never written, the narrowing lives elsewhere. (`SelectionVector` already exists in P1 with an identity fast path and `Compose`/`Slice`, ready for that use.)

## Alternatives Considered

**Executor-allocated, source-fills (copy-in) — `void GetChunk(DataChunk& out)`.**
The executor owns a reusable buffer and the source writes into it. *Rejected:* it forces a copy on every batch, defeating the point of a columnar store whose chunks are already resident in the exact layout we want to scan. The borrow interface lets `Scan` be literally zero-copy. The price — a lifetime footgun — is documented and localized to retaining sinks, which are few.

**Ownership transfer — return `DataChunk` by value or `unique_ptr<DataChunk>`.**
Hand the sink a chunk it owns. *Rejected:* a value return forces a move/allocation per batch even when the sink only reads-and-discards (the common streaming case: Filter, Project, any non-breaking operator — and `DataChunk`/`Vector` are move-only per ADR 0005, so a by-value return is a move of owning buffers, not free). It also makes the zero-copy scan impossible, because the table's resident chunk cannot be "given away." The borrow makes the cheap case cheap; retaining sinks pay for copies only when they actually retain.

**Pull-based / Volcano `next()` iterators.**
Each operator pulls from its child via a virtual `next()`. *Rejected in ADR 0001* (cited there: Neumann, *Efficiently Compiling Efficient Query Plans for Modern Hardware*, PVLDB 2011; Boncz, Zukowski, Nes, *MonetDB/X100*, CIDR 2005); restated here only to note that the *interfaces* in this ADR are the push counterpart — the loop lives in the `Pipeline`, not inside an operator pulling on a child.

**Source pushes by calling the sink directly (`source.Run(sink)`).**
Let the source own the loop and call `sink.Consume`. *Rejected:* it puts control flow back inside an operator, contradicting ADR 0001's "the loop owner in push is the executor, not the source operator." Keeping the loop in the `Pipeline` is what makes morsel-driven parallelism (below) a change to the *driver*, not to every source.

## Consequences

**Wins**

- **Zero-copy scan (by design).** `Scan` will return pointers into the table's own chunks; the hot path does no allocation and no copy across the Source→Sink boundary.
- **Cheap streaming default.** Non-retaining operators (the P3 Filter/Project, future maps) read the borrow and pass it on with no per-batch ownership churn — important given ADR 0005's move-only vectors, where "pass it on" must not mean "move/copy buffers."
- **Loop in one place.** Control flow lives in `Pipeline::Run`, matching ADR 0001's decided direction; operators are pure data transformers.
- **Parallelism-ready (design intent, P8).** See below — push + a single externalized loop generalizes to morsel-driven execution without changing operator interfaces.

**Tradeoffs (stated honestly)**

- **(a) Borrow-until-next-call is a real footgun.** Any sink that retains data *must* deep-copy before the next `GetChunk()` or it dangles. The contract is unforgiving; it will be enforced by convention and by `ResultCollector` doing the right thing, not by the type system. This is the cost accepted in exchange for zero-copy.
- **(b) No early-termination signal in P2.** `LIMIT` / Top-N want to tell the executor "stop pushing." That back-channel (a return code from `Consume`, or a sink-queryable `done` flag — ADR 0001 sketches it as an `OperatorResult::FINISHED` the executor checks between morsels) is **not part of P2**; the loop drains the source unconditionally. This is the exact push tradeoff ADR 0001 already called out as "the single sharpest ergonomic regression versus pull" — acknowledged here, deferred, not solved.
- **(c) `const` inputs push allocation onto transforms.** Because inputs are read-only, a transform that changes data must allocate and own its output chunk (or emit an out-of-band selection — DataChunk has no selection slot, see above). That cost is real and lands in P3 (Filter via selection vector, Project via a fresh chunk); P2 has no transforms, so pays nothing yet.
- **(d) "Where is the loop?" is non-obvious.** This is the classic pull→push inversion ADR 0001 flagged. A reader expecting Volcano `next()` will look inside the operators for the driving loop and not find it; it is in `Pipeline::Run`. The inversion is the whole point, but it has a learning cost.

## Forward-compatibility: morsel-driven parallelism (P8, Leis et al., *Morsel-Driven Parallelism*, SIGMOD 2014 — design intent, not built)

The single-threaded `Pipeline::Run` is the degenerate one-worker case of morsel-driven execution. The generalization:

- N worker threads each run the same loop. The scheduler **dispatches morsels** to workers (workers are *handed*, or *grab via the work-stealing dispatcher*, a morsel — they are not pulling in the Volcano sense; the executor still drives, consistent with this being the push model). A **morsel** is a unit of *input* — in Leis et al. typically tens-of-thousands of rows, **far larger than a 2048-value `DataChunk`** — which a worker re-chunks internally into `DataChunk`-sized batches and pushes through the **same operator chain**. (Morsel ≠ chunk; ADR 0001 keeps these distinct and so do we.)
- The chain ends in a **shared sink**. The parallel sink contract is richer than P2's single `Finalize()`: each worker accumulates **thread-local** partial state via `Consume` (and a per-worker local finalize / `Combine`), and a single **global merge** combines those partials (e.g. partition-wise combine of per-worker hash tables, k-way merge of per-worker sort runs). ADR 0001 is explicit that **sink finalization — not just source exhaustion — is the synchronization point** and is the genuinely hard part the morsel model introduces.

Leis et al.'s headline contribution is **NUMA-local, work-stealing morsel dispatch and scheduling** — not merely "run more threads." Push makes parallelizing natural — operators are stateless-per-batch transformers over `const` inputs, so parallelism means *running more drivers*, not rewriting operators — but the scheduling/locality machinery is the substance of the cited paper, and it is what P8 must build. Per ADR 0001's honest hardware caveat, on the single-NUMA Apple M3 Pro the NUMA dimension is largely moot; what is actually exercised on-device is **dynamic load balancing and cache locality**, with the genuine NUMA-dispatch story validated on the x86_64 CI runner. **None of P8 is built**; this section records the design intent the P2 interfaces are shaped to accommodate (a shared sink with a two-step local-finalize-then-global-merge, an externalized loop, const-input transforms).

## How to defend this at a whiteboard

- "First, honesty: P2 isn't built — the module map says `—`. This ADR fixes the **interfaces** P2 gets built against, in the same forward-design voice ADR 0004 used to spec the sort operator before P3. I'll say 'will return,' not 'returns.'"
- "The Source→Sink boundary is a **borrow, not a transfer**. `GetChunk()` returns a `const DataChunk*` the source owns, valid only until the next call. That's what lets `Scan` hand back pointers straight into the table's chunks — zero copy. The source also skips empty internal units, so the `Pipeline` loop just checks for `nullptr`."
- "The footgun is the flip side: a sink that **retains** must deep-copy. `ResultCollector` deep-copies, including pulling long-`StringRef` bytes into its **own** `StringHeap`. The reason is **ownership**, not relocation — ADR 0004 guarantees heap bytes never move, so the borrowed pointer doesn't dangle *while the source heap lives*, but the collector can't assume the source heap outlives it, so it owns its own copy."
- "Inputs are `const` because nothing downstream mutates them — Filter narrows with a selection vector, Project writes a fresh chunk. And `DataChunk` has **no selection-vector field**, so a P3 Filter emits its selection out-of-band, not by mutating the borrowed chunk. Transforms allocate their own output; that cost lands in P3."
- "The loop is in the `Pipeline`, not the operators — `while ((c = source.GetChunk())) sink.Consume(*c); sink.Finalize();`. That **realizes** ADR 0001's push direction; I'm not claiming it's a literal copy — I renamed `GetData`→`GetChunk` and split ADR 0001's one `Sink(chunk)` into per-batch `Consume` plus one-shot `Finalize`, because a breaker needs 'absorb' and 'complete' as separate events."
- "I rejected copy-in (`GetChunk(out&)`) because it copies every batch and kills the zero-copy scan; I rejected by-value/`unique_ptr` return because it forces a move per batch even for read-and-discard operators, and my vectors are move-only owning buffers."
- "Two honest gaps in P2: no early-termination signal — `LIMIT` can't yet say 'stop pushing,' which is ADR 0001's sharpest push regression; and no transform operators, so the const-input allocation cost isn't paid until P3."
- "It generalizes to morsel-driven parallelism (Leis et al., SIGMOD 2014): workers are **dispatched** morsels — input partitions much bigger than a 2048 chunk, re-chunked internally — and push them through the same chain into a shared sink. But the paper's real content is **NUMA-aware, work-stealing dispatch**, and the parallel sink needs per-worker local finalize plus a global merge — `Finalize()`-called-exactly-once only holds for one worker. On the single-NUMA M3 it's load-balancing and locality I exercise, not NUMA."
- "Same control-flow *direction* as DuckDB — push, executor-driven — but I'm not claiming API-level equivalence; these are Strata's own interfaces."
