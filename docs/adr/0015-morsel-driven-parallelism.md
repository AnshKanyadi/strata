# 0015. Morsel-driven parallelism: a work-stealing thread pool, thread-local aggregation, and a merge

## Status

**Accepted — primitives and the parallel-aggregation path built and tested; not
yet wired into the SQL executor.**

This is the honest, as-built status, and it is narrower than "as-built for P8" on
purpose. What is **built and tested in-repo today**:

- the persistent **work-stealing thread pool** with `ParallelFor`
  (`include/strata/parallel/thread_pool.hpp`, `src/parallel/thread_pool.cpp`,
  `tests/test_thread_pool.cpp`);
- the per-aggregate **COMBINE** op (the `combine` function pointer in
  `ResolvedAggregate`, `aggregate.hpp:47`, with `CombCount`/`CombSum`/
  `CombMinMax`/`CombAvg` in `src/exec/aggregate.cpp`) and state-level
  **`HashAggregate::MergeFrom`** (`src/exec/hash_aggregate.cpp:273`);
- the **morsel-driven parallel-aggregation driver** itself —
  `ParallelAggregate` (`src/parallel/parallel_aggregate.cpp`,
  `tests/test_parallel_aggregate.cpp`): N thread-local `HashAggregate`s, a
  `ParallelFor(num_chunks, …)` morsel-per-chunk dispatch, a post-barrier
  `MergeFrom` fold, and a final `Finalize`. Its test asserts the parallel result
  is **bit-identical to the serial `HashAggregate`** for integer aggregates at
  `nthreads ∈ {1, 2, 4, 8}`.

What is **NOT yet built**, and is therefore explicitly *not* claimed here:

- **No executor wiring.** `src/plan/executor.cpp` still builds a single serial
  `HashAggregate` and drives it through one `while (scan.GetChunk())` loop. The
  parallel path (`ParallelAggregate`) is reachable only by calling it directly,
  which today only the tests do. An end-to-end SQL query does not yet run
  parallel.
- **No inline N=1 fast path.** `ThreadPool(1)` spawns one worker thread and still
  dispatches through the mutex/cv machinery (see Decision §1).
- **The 1→N scaling curve is now measured** for the standalone `ParallelAggregate`
  operator and recorded in `docs/BENCHMARKS.md` (≈3.7× at 4 threads, 5.4× at 6,
  8.1× at 11 on the M3 Pro — sub-linear, as predicted). It is *not* measured
  through the SQL executor, which still aggregates serially.
- **Morsels are single chunks, not chunk-ranges** (Decision §2).
- **Parallel filter/project and parallel join** are foreshadowed, not built
  (Scope).

The bar set for everything that *is* built is **ThreadSanitizer-clean** (ADR
0002's `tsan` preset exists precisely for this), so the memory-model reasoning
behind *every* shared-state access is part of the decision, not an afterthought.

## Context

P8 is the marquee deliverable foreshadowed since ADR 0001: turn the push-based
engine parallel using the **morsel-driven** model of Leis, Boncz, Kemper,
Neumann (*Morsel-Driven Parallelism: A NUMA-Aware Query Evaluation Framework for
the Many-Core Age*, SIGMOD 2014). This ADR records the as-built design of three
pieces and how they fit: (1) a persistent **work-stealing thread pool** with a
`ParallelFor` dispatch primitive, (2) **morsel-driven execution** of a scan
sliced into chunk units, and (3) the showcase parallel operator — **parallel
aggregation** with N thread-local hash tables and a single-threaded
**state-level merge**.

Strata is push-based by ADR 0001, and that ADR already named morsel-driven
parallelism as the **decisive reason** for choosing push over pull: a push
pipeline is *already* "a function from a source batch to side effects on a sink"
(ADR 0001 §"decisive reason", verbatim), so making it parallel is — paraphrasing
0001 — hand a worker a morsel, let it push the morsel through the fused operator
chain, synchronize only at the sink. ADR 0001 also fixed the synchronization
point, verbatim: a pipeline is complete only when source morsels are exhausted
*and* the sink's parallel state is combined/finalized — **"sink finalization —
not just source exhaustion — is the synchronization point"** (ADR 0001 line 47,
em-dashes as in the original). P8 is the cash-in of that promise. This ADR's
whole shape (parallel phase → barrier → single-threaded merge → finalize) is ADR
0001's sink-finalization sentence made concrete.

The Leis et al. model, stated accurately so we cite it honestly:

- **Morsels.** The input is sliced into small, fixed-size *morsels* (the paper
  uses ~10,000 tuples). A worker grabs one morsel, runs it through the pipeline,
  reports into shared/thread-local state, and grabs the next. Morsels are the
  unit of *dynamic* scheduling — the degree of parallelism is **not** baked into
  the plan (contrast the Volcano exchange-operator approach, which freezes
  parallelism into the plan shape).
- **NUMA-aware morsel assignment with work-stealing as the load-balancing
  mechanism.** The paper's *headline* contribution is **NUMA-local** morsel
  assignment: morsels reference memory on a given socket and are preferentially
  dispatched to a worker on that socket. The dispatcher itself is a
  range-based, lock-free work-stealing scheduler — work-stealing is the **core
  dispatch mechanism** the paper uses to keep all cores busy, *combined with*
  NUMA-aware placement (not merely a drain-time fallback that engages only after
  a local queue empties). We will not subordinate work-stealing more than the
  paper does.
- **Thread-local state + merge.** Pipeline-breaking operators (aggregation, join
  build) accumulate into per-worker partial state, which is combined at the
  pipeline barrier. This is the parallel correctness story for stateful
  operators.

**The honest hardware framing (ADR 0001's caveat, restated and load-bearing
here).** The dev machine is an Apple M3 Pro: 5 performance + 6 efficiency cores
(11 total, no SMT), arm64, **unified memory = effectively a single NUMA domain**.
On this machine the NUMA dimension of Leis et al. is **moot** — there is one
memory domain, so "NUMA-local morsel placement" has nothing to localize. What we
genuinely realize from the morsel model on-device is **dynamic load balancing**
(a fast worker steals work a slow worker hasn't gotten to) and **cache locality**
(a morsel-sized working set stays warm). The cores are also **asymmetric** — the
6 E-cores are materially slower than the 5 P-cores — which is exactly why static,
equal partitioning would stall on the slowest worker and why dynamic dispatch is
the right primitive even with NUMA out of the picture.

**NUMA-local dispatch is validated nowhere in this project as it stands**, and we
say so plainly. It is moot on the M3's single unified-memory domain; and the
x86_64 CI leg is a standard hosted GitHub `ubuntu-latest` runner — a shared,
effectively single-socket VM (per `BENCHMARKS.md`), which gives no real
cross-socket NUMA topology to validate NUMA-local placement against either.
NUMA-locality is a property of the *paper's* target hardware (a real multi-socket
many-core box), deferred to genuine multi-socket hardware. (ADR 0001's line
"the genuine NUMA story is validated on the multi-socket-style behavior of the
x86_64 CI runner" overclaims on this point and should be reconciled down; this
ADR does not inherit that claim.) We will not claim a NUMA win we cannot get on
any hardware we actually run on.

**What P8 must produce.** The same answer the serial path produces, parallel, and
TSan-clean. "The same answer" needs the careful definition given under
Determinism below: a SET of groups identical to serial, bit-identical per-group
*integer* aggregates, and an honestly-stated last-ULP caveat for floating point.

## Decision

### 1. A persistent work-stealing thread pool with a `ParallelFor` primitive

We build **N persistent worker threads**, spun up once in the `ThreadPool`
constructor and reused for the life of the pool (not spawned per query — thread
creation is far too expensive to pay per dispatch). **N is a constructor
argument; callers pass `std::thread::hardware_concurrency()`** (11 on the M3 Pro)
for the full-parallelism case. **`ThreadPool(0)` is bumped to 1** in the
constructor.

**On N = 1 — stated as-built, no fast path.** `ThreadPool(1)` is correct and
serial-in-effect, but it is **not** an inline-on-the-caller fast path: the
constructor still spawns one worker thread, and `ParallelFor` still goes through
`Submit` (taking a deque mutex) and still blocks the dispatcher on `done_cv_`
under `done_mu_`. So N=1 runs every task on that single worker thread through the
full dispatch machinery — correct and race-free, but it pays the mutex/cv cost.
There is **no `if (size()==1) run body(i,0) inline and skip the pool` branch in
the code today.** The `tests/test_thread_pool.cpp` "SingleThreadIsSerial" test
confirms exactly this: at N=1 every task runs with `worker_id == 0`, in order, on
the one worker. (An inline N=1 fast path is an easy, un-taken optimization, noted
under Tradeoffs.)

The pool is **work-stealing** in the Leis sense:

- **Each worker owns its own task deque, guarded by its own mutex** (one
  `std::mutex` per `Worker`, held in a `unique_ptr` so the mutex is never moved).
  A worker runs its *own* deque **LIFO** (push back via `Submit` / pop back in
  `TryGetTask` — the freshest task is the hottest in cache, the classic
  work-stealing discipline). When its own deque is empty, an idle worker **steals
  FIFO** from another worker's deque (pop front — it takes the *oldest* task, the
  one least likely to be that victim's hot next-task, minimizing contention on
  the victim's LIFO end).
- **A condition variable parks idle workers.** A worker that finds its deque
  empty and fails to steal blocks on the pool's `work_cv_` (predicate: `stop_ ||
  available_ > 0`) rather than spinning, so idle cores are not burned (this
  matters on a battery-powered laptop and is friendlier to the E-cores).

**`ParallelFor(num_tasks, body)` is the one dispatch primitive.** It pushes
`num_tasks` tasks round-robin across the workers' deques (`Submit(i % n, …)`),
wakes parked workers, and **blocks the calling (dispatcher) thread until all
`num_tasks` tasks have completed**, using an **atomic completion counter**
(`outstanding_`) plus `done_cv_`. Crucially, the callback signature is
**`body(task_index, worker_id)`** — the task index first, then the id of the
worker *actually running it* (`thread_pool.hpp:30`; the worker loop calls
`t(id)`, and `ParallelFor`'s wrapper forwards it as `body(i, worker_id)`,
`thread_pool.cpp:95`). The task indexes its thread-local state by `worker_id`,
the id of the worker actually running the task. This worker-id hand-off is the
linchpin of the lock-free thread-local accumulation story below. **It is also
documented (`thread_pool.hpp:31`) as not re-entrant / not called concurrently**
— see the nested-task-graph note under Alternatives.

`ParallelFor` is a **fork-join barrier**: dispatch → all tasks run on the pool →
join. This is the synchronization point ADR 0001 named.

### 2. Morsel-driven execution of the scan

A scan's input is the `ColumnarTable` (ADR 0007), a sequence of `<= kVectorSize`
(2048-row) chunks. As built, **a morsel = one table chunk**: the parallel
aggregation driver does `ParallelFor(table.chunk_count(), …)` and each task
folds `table.chunk(m)` into a thread-local accumulator
(`parallel_aggregate.cpp:37`). A chunk is aligned to the existing chunk boundary
so no chunk is split across workers and the existing per-chunk kernels are
untouched. (Coarsening a morsel to a *range* of chunks — fewer, larger tasks —
is a trivial future refinement; today the unit is one chunk.)

`ParallelFor(num_chunks, …)` **dispatches one task per chunk**. Work-stealing
gives the **dynamic load balancing** that is the on-device point: a worker that
finishes its chunks early steals unstarted chunks from a worker that is behind —
which, on the asymmetric M3, is exactly how the 5 P-cores absorb work the 6
E-cores haven't reached, instead of every worker stalling on the slowest. Each
worker pushes its chunks through into its **own thread-local sink state**. After
the `ParallelFor` barrier, a single-threaded **merge/combine** phase combines the
per-worker partials into the final result. (For a stateless pipeline —
filter/project to a result collector — the "merge" would be just an ordered
concatenation of per-worker output; that path is not built yet. The interesting
merge is aggregation, below.)

### 3. Parallel aggregation — the showcase (and the only operator that needs a real merge)

Aggregation is the hardest parallel operator because it is a pipeline breaker
*with mergeable state*: every worker must accumulate independently and then the
partials must be combined. The as-built design (`ParallelAggregate`,
`src/parallel/parallel_aggregate.cpp`):

- **N thread-local `HashAggregate`s.** A `std::vector<unique_ptr<HashAggregate>>`
  of size N, one per worker, each its **own** open-addressing hash table exactly
  as ADR 0010 specifies (slots + salt + row buffer `[hash | keys | null-flags |
  states]`, its own `StringHeap` for VARCHAR keys). There is **no shared,
  concurrent hash table** — we deliberately do not build one (see Alternatives).
  Each thread-local accumulator is constructed with a `NullSink` (it only
  accumulates; it is merged, never finalized). Worker `w` calls
  `locals[w]->Consume(chunk)` for every chunk in its morsels. This reuses the
  entire ADR 0010/0011 `HashAggregate::Consume` path verbatim, per worker, with
  zero locking inside the hot probe-and-scatter loop.

- **A state-level `MergeFrom` after the barrier.** Once `ParallelFor` returns,
  the dispatcher thread builds one **final `HashAggregate`** (wired to the real
  `output` sink) and folds accumulators `0..N-1` into it with
  `MergeFrom(other)` on `HashAggregate` (`hash_aggregate.cpp:273`): it walks
  every group row of `other`, probes the final table for that group's key
  (creating the group if absent, via `FindOrCreateGroupFromRow`, copying the key
  into the final table's own `StringHeap`), and **combines the raw partial
  states** group-wise. (Global, no-`GROUP BY` aggregation is the special case
  `MergeFrom` handles first: a single pre-created group 0 on each side, combined
  directly without touching the slots table.)

- **The merge is at the STATE level — raw partial states, never finalized
  values.** Each aggregate carries a per-aggregate **COMBINE** op — a third
  function pointer `combine` alongside `update`/`finalize` in `ResolvedAggregate`
  (`aggregate.hpp:47`), defined per state struct in `src/exec/aggregate.cpp`:
  - `CombCount` (`CountSt`): `count += other.count`.
  - `CombSum<A>` (`SumSt<A>`): `sum = AddOp(sum, other.sum); has = has ||
    other.has`.
  - `CombMinMax<T,IsMax>` (`MinMaxSt<T>`): if the source `has` and is the better
    extremum, take its `value`; `has = has || other.has`; an absent side
    (`!has`) contributes nothing.
  - `CombAvg<A>` (`AvgSt<A>`): combine the **(sum, count) pair** componentwise —
    `sum = AddOp(sum, other.sum); count += other.count`.
  COMBINE is the partial-merge analogue of `update`: where `update` folds one
  *input value* into a state, COMBINE folds one *partial state* into another.
  Both operate on the same struct layout, so COMBINE touches only the bytes ADR
  0010 already lays out inline in the row. (`CombSum`/`CombAvg` use the same
  wrapping `scalar::AddOp` as the serial `update` — see Determinism for why that
  is still bit-identical to serial.)

- **Why merge raw `(sum, count)` *before* `Finalize` — this is the crux.**
  `AVG` is stored as `(sum, count)` and divided **once at finalization** (ADR
  0011's `FinAvg`: `double(sum) / double(count)`). You **cannot** correctly
  recombine two *finalized* averages — `avg(A ∪ B) ≠ (avg(A) + avg(B)) / 2`
  unless the partitions are equal size, which morsels are not. The only correct
  merge keeps the unfinalized `(sum, count)` and adds them componentwise, then
  finalizes the *combined* pair once. This is precisely **why the merge is
  defined on raw state and `Finalize` runs last**: merge the partials, *then*
  finalize the single merged table to the output `DataChunk`s. The same argument
  applies to `MIN`/`MAX` (`has` must OR through the merge so an all-NULL partial
  doesn't masquerade as a real extremum) and to the NULL-on-empty semantics of
  ADR 0011 (an untouched partial state stays untouched through COMBINE and
  finalizes to NULL).

The final merged accumulator is `Finalize`'d exactly as in the serial path —
drained group-by-group into output chunks (ADR 0010's drain), spilling across
multiple chunks past 2048 groups, in hash order (unordered).

### 4. Why push (ADR 0001) made all of this natural

A push pipeline is already "a morsel in, side effects on thread-local sink
state." The parallel version changes *nothing* about the operator code in the hot
loop: the same `Consume(chunk)` runs, just on a per-worker sink. The only new
machinery is *outside* the operators — the pool, the morsel slicing, and the
merge — which is exactly where ADR 0001 said the parallelism would live ("the
scheduler's unit of work is a morsel through a pipeline, with synchronization
confined to sink state and its finalization"). The `ParallelFor` barrier **is**
that sink-finalization synchronization point. Had we chosen pull, the driving
loop would live at the root and we would be retrofitting thread-safe resume-state
or exchange operators onto stateful iterators — the friction the morsel model
exists to remove. P8 is the receipt for ADR 0001's central bet — though note the
receipt is, today, the `ParallelAggregate` *driver and its tests*, not yet the
SQL executor (Status).

## Memory-model reasoning (required: every shared-state access and its synchronization)

This is the part that makes the pool and the parallel-aggregation path
TSan-clean. We enumerate **every** piece of state touched by more than one thread
and name the synchronization that establishes the happens-before edge for it. The
C++ memory-model rules we lean on: (a) a mutex unlock *synchronizes-with* the next
lock of the same mutex; (b) an atomic release-store (or the release half of an
acq_rel RMW) *synchronizes-with* an acquire-load that reads the value written;
and crucially (c) the **release-sequence** rule — when an atomic is mutated by a
chain of read-modify-write operations, an acquire-load that observes the final
value synchronizes-with the *entire* release sequence, hence with the writes
before *every* RMW in the chain, not merely the last one. Everything a thread did
before a release is visible to a thread after the matching acquire.

1. **The per-worker task deques and the idle-park condition variable.**
   *All* access to a worker's deque — the owner's LIFO push/pop and any thief's
   FIFO steal — happens **under that deque's `Worker::mu` mutex**
   (`Submit`, `TryGetTask`). The lock/unlock pair establishes happens-before
   between any two accesses to the same deque, so there is no data race on the
   deque itself, and the *task object* a thief observes was fully published by the
   producer under the same lock. The park condition variable is waited on under
   `work_mu_` with a predicate loop (`work_cv_.wait(lk, predicate)`), and a
   producer closes the lost-wakeup window by taking and releasing `work_mu_`
   before `notify_one` (`thread_pool.cpp:36`); the destructor does the same
   before `notify_all` (`thread_pool.cpp:20`). This mutex-guarded access removes
   the data race **on the deque**; it is *not* the sole reason the scheduler is
   TSan-clean (see the closing paragraph).

2. **`ParallelFor` completion (the barrier).** `outstanding_` is an atomic
   counter: `Submit` does `fetch_add(release)`; each worker, after running its
   task, does `outstanding_.fetch_sub(1, acq_rel)` (`thread_pool.cpp:72`) and
   notifies `done_cv_` (again closing the lost-wakeup window under `done_mu_`
   first). The dispatcher blocks in `done_cv_.wait(lk, [...] {
   outstanding_.load(acquire) == 0; })` (`thread_pool.cpp:100`). Because the
   decrements are **acq_rel RMWs**, they form a release sequence on
   `outstanding_`; the dispatcher's acquire-load observing the final value `0`
   therefore synchronizes-with the **whole release sequence**, hence with **every
   task's prior writes — not merely the last decrement** (release-sequence rule
   (c) above). That is the precise reason **every memory effect of every task
   happens-before the dispatcher resumes**, and it is the edge the merge then
   relies on.

3. **The input `ColumnarTable`.** It is **read-only for the entire parallel
   phase** — no worker mutates it; the loader (ADR 0007) has finished long before
   dispatch. Concurrent reads of immutable data are race-free by definition and
   need **no synchronization at all**. Workers read disjoint chunks anyway, but
   even overlapping reads would be fine; the guarantee is immutability, not
   disjointness.

4. **The thread-local accumulators
   (`std::vector<unique_ptr<HashAggregate>>` of size N).** Worker `w` touches
   **only `locals[w]`**. These are **distinct objects** at distinct heap
   addresses, in a vector **sized once before dispatch and never resized** during
   the parallel phase (no reallocation; and because they are held by
   `unique_ptr`, even the controlling vector reallocating could not move a
   `HashAggregate` — its address is stable). Concurrent access to *different*
   accumulators is concurrent access to disjoint objects — **race-free with no
   synchronization**, because no byte is shared between two workers. The subtlety
   that makes this airtight under work-stealing: a task **stolen** by another
   worker accumulates into the **stealing** worker's accumulator (it indexes by
   the `worker_id` `ParallelFor` passes it — the worker *actually running* the
   task, via `t(id)` at `thread_pool.cpp:71`, not the task's origin). The
   guarantee this yields is stronger than "one writer at a time": **exactly one
   writer per accumulator for the entire parallel phase** — accumulator `w` is
   written only by worker `w`, and worker `w` runs its tasks **sequentially**
   within its loop — so never two threads, and never even single-thread
   alternation between two writers on one accumulator. (This is also why a morsel
   must finish atomically on one worker — it does; a single task runs to
   completion on whichever worker dequeued it.) Adjacent accumulator headers
   could share a cache line in principle, but each is a separate heap allocation,
   so false sharing is at most a performance note, not a race.

5. **The merge.** `MergeFrom` reads **all N partials single-threaded, strictly
   AFTER the `ParallelFor` barrier**. The merge runs on the **dispatcher thread**
   — the thread that called `ParallelAggregate`/`ParallelFor` — which is **not**
   one of the pool worker threads and did not itself participate in the parallel
   phase. It nonetheless observes every worker's writes via the **`outstanding_`
   acquire-load that `ParallelFor` performed before returning (item 2)**; that
   single acquire edge — synchronizing-with the whole release sequence of
   decrements — is the *entire* reason no merge-side lock is needed. The merge is
   ordinary single-threaded code reading data the barrier already published, and
   it owns the lifetime from here through `Finalize`.

The shape generalizes: **all sharing is either (a) immutable (the input table),
or (b) mediated by a mutex / atomic-counter happens-before edge (the deques, the
park cv, the barrier), or (c) partitioned into per-worker-disjoint objects (the
accumulators).** There is no plain (non-atomic) read of data another thread is
concurrently writing. **TSan-cleanliness is the *conjunction* of all three** — the
deque mutexes (item 1), the `outstanding_` release/acquire release-sequence edges
(items 2 and 4's hand-off), and the predicate-loop cv discipline (items 1, 2) —
not any single mechanism. (The header comment "TSan-clean by construction" is
shorthand for this conjunction.)

## Alternatives Considered

### A lock-free (Chase-Lev) work-stealing deque
**Rejected for P8 — explicitly deferred, not dismissed.** The Chase-Lev
work-stealing deque (the canonical lock-free design: owner does relaxed push/pop
on one end, thieves CAS-steal from the other) is *the* known throughput
optimization for a work-stealing pool, and a real engine would want it. We chose
**mutex-guarded deques** instead, prioritizing **correctness and
TSan-cleanliness** over the last increment of scheduler throughput. A correct
lock-free deque requires subtle acquire/release fences and a CAS race between a
steal and the owner's pop on the last element; getting the memory orderings
exactly right *and* proving them clean under TSan is a project in itself. Our
dispatch granularity is a whole chunk (thousands of rows of real work), so deque
operations are rare relative to the work per task — the mutex is not on any hot
path that matters. The lock-free deque is recorded as the deferred optimization
with a clear-eyed note that it buys scheduler throughput we are not currently
bottlenecked on.

### A single shared atomic morsel cursor (no per-worker deques at all)
**Considered, and honestly the simpler design that would also work.** A single
`std::atomic<size_t>` morsel index that every idle worker `fetch_add`s to claim
the next morsel gives **dynamic load balancing** — the property we actually
realize on-device — with far less code than per-worker deques + stealing, and it
is trivially TSan-clean (one atomic). We chose **per-worker deques + stealing**
anyway because it is **the mechanism Leis et al. name** and the project is as much
a study guide as an engine — implementing the named mechanism is part of the
point. A deque-per-worker structure is also what *would* generalize to
nested/dependent task graphs (a task spawning sub-tasks onto its own deque) in a
fuller scheduler — **but the as-built `ParallelFor` does not support that today**:
it is a single non-reentrant fork-join barrier with one shared `outstanding_`
counter and one `done_cv_`, and the header marks it "not re-entrant / not called
concurrently" (`thread_pool.hpp:31`). A task that called `ParallelFor` again, or
spawned sub-tasks tracked by the same `outstanding_`, would corrupt the barrier
(the inner batch's completion would mis-count the outer wait). So the
"generalizes to nested task graphs" property is true of Chase-Lev-style
schedulers *in the abstract*, not of this implementation. We keep the per-worker
deques for the paper's named-mechanism pedagogy, not because our current barrier
exploits their generality. And we state plainly: **the per-NUMA-queue placement
that *motivates* work-stealing in the paper is moot on the M3's single
unified-memory domain**, so the benefit we genuinely get from stealing over a
shared cursor is *load balancing*, which the shared cursor would also give. We are
not claiming a NUMA-locality advantage we cannot realize on this hardware. (On a
real multi-socket box the per-queue structure is where the NUMA story would
re-enter; that is future hardware, not an on-device claim.)

### A single shared concurrent (lock-free / latched) aggregate hash table
**Rejected.** One global hash table that all workers probe-and-update
concurrently would avoid the merge phase entirely, but it pays for it with
per-update synchronization (a latch per bucket, or a lock-free insert/update
protocol with CAS on every group state) on the **hottest random-access structure
in the engine** (ADR 0010), which is already memory-latency bound (ADR 0008).
Contention on hot groups (low-cardinality `GROUP BY` — the common TPC-H case,
e.g. `GROUP BY l_returnflag, l_linestatus` ≈ 4 groups) would serialize exactly
the workers we are trying to parallelize. The thread-local-table + merge design
keeps the hot loop **lock-free by construction** (one writer per table) and pays a
**bounded, single-threaded merge** cost proportional to total group cardinality
(small for low-cardinality grouping; the partials are also small). This is the
Leis et al. recommendation and the standard analytical-engine choice. The merge
is a feature (it is *the* parallel-aggregation teaching moment), not a tax we are
ashamed of.

### Static, equal partitioning of the input across N workers (no morsels, no stealing)
**Rejected.** Slicing the table into N equal contiguous ranges, one per worker,
is the simplest possible parallelism and needs no scheduler. It fails on the M3
precisely because the cores are **asymmetric**: an equal split gives the slow
E-cores the same row count as the fast P-cores, and the whole `ParallelFor`
blocks on the slowest worker (and on any data skew — a range that happens to be
all-matching after a filter does more downstream work). Morsels + dynamic
dispatch exist to fix exactly this; with morsels far more numerous than workers,
the slow cores simply claim fewer morsels and no worker idles waiting for a
straggler.

### A thread pool spun up per query (or per `ParallelFor`)
**Rejected.** Thread creation/teardown costs microseconds-to-more each and would
dominate short queries; persistent workers parked on a condition variable cost
nothing while idle and wake in nanoseconds. Persistent + parked is the standard
choice. (Note: this argues for *persistence*, not for an N=1 fast path — at N=1
the as-built pool still pays the dispatch mutex/cv cost; see Decision §1 and
Tradeoffs.)

## Consequences

### Wins (accepted benefits)
- **The hot loop stays exactly the serial hot loop.** Per-worker
  `HashAggregate::Consume` is the unmodified ADR 0010/0011 path — no locks, no
  atomics, no contention inside the probe-and-scatter. All parallel machinery
  lives outside the operators, as ADR 0001 predicted.
- **Lock-free-by-construction aggregation** without a lock-free data structure:
  one writer per thread-local table, correctness by disjointness, a single
  bounded merge.
- **Dynamic load balancing that fits the asymmetric M3.** Slow E-cores claim
  fewer morsels; fast P-cores steal the rest; no worker blocks on a straggler.
- **TSan-clean by a small, enumerable set of synchronization edges** (the
  memory-model section is the proof). The `tsan` preset (ADR 0002) is the gate;
  `test_thread_pool.cpp` and `test_parallel_aggregate.cpp` are the tests that run
  under it.
- **A correct, validated parallel-aggregation path.** `ParallelAggregate` is
  tested **bit-identical to serial** for integer aggregates at `nthreads ∈ {1, 2,
  4, 8}`, including a many-thread/many-row stress test.
- **The mechanism generalizes (structurally).** `ParallelFor` + per-worker
  thread-local state + merge is the template for parallel filter/project (no
  merge) and the future parallel join (ADR 0001/P9) — modulo the non-reentrancy
  caveat above.

### Tradeoffs and costs (stated honestly)
- **Not wired into the SQL executor.** The big one. `src/plan/executor.cpp` still
  runs aggregation serially through a single `HashAggregate`; `ParallelAggregate`
  is exercised only by its test. End-to-end parallel query execution is the next
  integration step, not a done thing — which is exactly why Status is "primitives
  + path built and tested, not yet wired," not "as-built for P8."
- **No inline N=1 fast path.** At N=1 the pool spawns one worker and dispatches
  through the mutex/cv machinery; it does not run tasks inline on the caller. A
  guarded inline path (`if (size()==1) run body(i, 0) directly`) is an easy,
  un-taken optimization. We describe N=1 as "one worker thread; correct and
  serial, but still pays the dispatch mutex/cv," not as a zero-overhead serial
  path.
- **Mutex-guarded deques, not lock-free.** We leave scheduler throughput on the
  table by choice; the Chase-Lev deque is deferred. Defensible because dispatch is
  per-chunk (coarse), so the mutex is off the hot path — but it *is* a known,
  un-taken optimization.
- **No NUMA win, and we don't claim one anywhere.** The work-stealing structure's
  NUMA-locality rationale is inert on the M3's single memory domain *and* on the
  single-socket CI VM; what we realize is load balancing and cache locality. A
  shared atomic morsel cursor would have delivered the same benefit more simply —
  we chose the paper's named mechanism deliberately and say so.
- **Scaling will be sub-linear, and we will MEASURE and REPORT it rather than
  claim near-linear.** Two compounding reasons: (1) the M3's cores are
  **asymmetric** — 5 fast P-cores + 6 slow E-cores, so the 11th "core" of work is
  worth far less than the 1st; and (2) aggregation is **memory-/hash-probe-bound**
  (ADR 0008's ceiling: the probe is gather/pointer-chase, memory-latency bound,
  not lane- or compute-bound), so adding cores quickly saturates shared memory
  bandwidth and the speedup curve flattens **well below 11×**. The **measured**
  1→N curve for the standalone `ParallelAggregate` is now in `BENCHMARKS.md`:
  near-linear to ~4 threads, 5.4× at 6 (≈ the 5 P-cores), and **8.1× at 11** — not
  ~11×, exactly the asymmetric-core sub-linearity predicted here. It is measured on
  the standalone operator, **not** through the SQL executor (which still aggregates
  serially). The curve is the honest artifact, not a headline multiplier.
- **Determinism — precise, not hand-waved.** The parallel result equals the
  serial result **as a SET of groups**; the group *output order* differs with
  thread interleaving, which is fine because `GROUP BY` output is unordered anyway
  (any required order is a downstream `ORDER BY`, ADR 0013). For **integer**
  `SUM`/`COUNT`/`MIN`/`MAX`, the per-group values are **bit-identical to serial**.
  The reason, stated to survive the overflow objection: our integer `SUM`/`AVG`
  use **wrapping two's-complement addition** (`scalar::AddOp`, ADR 0011 — int64
  sum can overflow by design), and addition **modulo 2⁶⁴ is associative and
  commutative**, so reordering the adds across threads — and across the
  COMBINE-vs-update split — cannot change the resulting bit pattern *even on
  overflow*; `COUNT` is plain integer addition; `MIN`/`MAX` are order-independent.
  We therefore **test bit-identical equality on integer aggregates**
  (`test_parallel_aggregate.cpp`). For **`SUM(double)` and `AVG(double)`**, the
  cross-thread summation order differs from serial and **IEEE-754 addition is
  non-associative**, so parallel results may differ from serial **in the last
  ULP**; we **state that caveat plainly** and compare doubles within an epsilon.
  We use **no compensated/Kahan summation** — a deliberate non-decision recorded
  honestly, consistent with ADR 0011's FP-order note; adding it would narrow but
  not eliminate the order dependence and is not in P8. **Integer `AVG` is
  correctly excluded from the bit-identical set**: it outputs `double`, and ADR
  0011 flags that converting an `int64` sum > 2⁵³ to `double` before dividing
  loses low bits regardless of thread order.
- **A real, single-threaded merge cost.** The merge is `O(total distinct groups)`
  probe-and-combine work on one thread after the barrier. It is bounded and small
  for low-cardinality grouping (the common case), but for very-high-cardinality
  `GROUP BY` it is a non-trivial serial tail that eats into the parallel speedup
  (a partition-and-merge or radix-partitioned aggregation would parallelize the
  merge too — deferred). We will report this in the scaling curve rather than hide
  it.

### Scope: what is built, and what is foreshadowed but not built
- **Parallel aggregation is built and tested as `ParallelAggregate`** (the
  showcase), but **not yet driven from the SQL executor** (Tradeoffs).
- **Parallel filter/project are embarrassingly parallel** — per-morsel, **no
  merge** — and would run through the same `ParallelFor`. **Not built yet**; the
  primitive exists, the operator path through it does not.
- **Parallel JOIN is foreshadowed, not built.** It lands with **join execution in
  P9**: thread-local / partitioned build + a merge of build state, then a
  **read-only parallel probe** (the build hash table is immutable during the
  probe phase — race-free reads, item 3's pattern again). P8 deliberately does not
  build it; this ADR records the intended shape so the `ParallelFor` + merge
  template is seen to generalize, without claiming P9 is done.

## How to defend this at a whiteboard

- "**Morsel-driven parallelism**, Leis, Boncz, Kemper, Neumann, **SIGMOD 2014**.
  Slice the input into small fixed-size **morsels**; persistent workers grab
  morsels, push each through the pipeline, accumulate into **thread-local** state;
  combine at the pipeline barrier. The paper's headline is **NUMA-aware morsel
  assignment with work-stealing as the load-balancing mechanism** — and I'll be
  honest that the NUMA half is moot on my M3's single unified-memory domain (and
  on the single-socket CI VM), so NUMA-local dispatch is validated *nowhere* in
  my project; what I actually realize is **load balancing and cache locality**."
- "**Honest status first:** the **building blocks all exist and pass tests** — the
  work-stealing pool with `ParallelFor`, the per-aggregate COMBINE op,
  `HashAggregate::MergeFrom`, and a `ParallelAggregate` driver that's
  bit-identical to serial at 1/2/4/8 threads, with a measured 1→N scaling curve
  (8.1× at 11 threads, sub-linear, in BENCHMARKS.md). What's **not done** is wiring
  it into the SQL executor — a plain query still runs aggregation serially. So I'd
  call it 'primitives + the parallel-aggregation path built and measured,
  integration pending,' not 'fully shipped.'"
- "**Push (ADR 0001) made this natural** — a push pipeline is already 'a morsel
  in, side effects on a thread-local sink.' The hot `Consume` loop is the
  unchanged serial loop; all the parallel machinery — pool, morsel slicing, merge
  — lives *outside* the operators. The `ParallelFor` barrier is the 'sink
  finalization — not just source exhaustion — is the synchronization point' that
  ADR 0001 promised."
- "**The pool:** N persistent workers (caller passes `hardware_concurrency`, 11
  here). Each worker owns a **mutex-guarded deque**, runs it **LIFO**, **steals
  FIFO** from others when idle, parks on a **condition variable**.
  `ParallelFor(num_tasks, body)` spreads tasks round-robin, wakes workers, and
  **blocks on an atomic completion counter + cv** until all done. The callback is
  **`body(task_index, worker_id)`** — task index first, then the id of the worker
  actually running it, so it indexes thread-local state by the running worker.
  **N=1 is correct and serial but I won't oversell it** — it still spawns one
  worker and dispatches through the mutex/cv; there's no inline fast path in the
  code today."
- "**Why not lock-free?** A **Chase-Lev** deque is the known optimization; I chose
  **mutex-guarded deques** for correctness and **TSan-cleanliness**, and dispatch
  is per-chunk so the mutex is off the hot path. Lock-free is **deferred**, on
  purpose. A **single shared atomic morsel cursor** would also give load balancing
  more simply — I implemented the paper's named mechanism deliberately. I'd add
  that the deque structure is what *would* generalize to nested task graphs, but
  my current `ParallelFor` is a single non-reentrant fork-join barrier, so it
  doesn't exploit that yet."
- "**Parallel aggregation is the showcase** because it's the hard one — it needs a
  **merge**. **N thread-local hash tables** (each the ADR 0010 open-addressing
  table, its own `StringHeap`), one writer each, **no shared concurrent table**.
  After the barrier, one thread builds a final table and **`MergeFrom`** folds
  them in."
- "**The merge is at the STATE level, before `Finalize`** — and that's the crux.
  Per-aggregate **COMBINE**: `SUM += SUM` (and `has |=`), `COUNT += COUNT`,
  `MIN`/`MAX` take the better and OR `has`, `AVG` adds the **`(sum, count)`
  pairs**. You **cannot recombine a finalized AVG** — `avg(A∪B) ≠ (avg A + avg
  B)/2` for unequal partitions — so I merge raw `(sum, count)` and **finalize once
  at the end**. That single fact is why merge runs on raw state and `Finalize`
  runs last."
- "**Memory model — the TSan story is the conjunction of three things, not one.**
  Sharing is exactly three kinds. (1) The **deques + park cv** — all under each
  deque's **mutex**, plus predicate-loop cv discipline. (2) **`ParallelFor`
  completion** — an **acq_rel** atomic counter; because the decrements are RMWs
  they form a **release sequence**, so the dispatcher's acquire-load of 0
  synchronizes-with **every** task's writes, not just the last decrement — that's
  what lets the merge see everything. (3) The **input table is read-only** —
  immutable concurrent reads need no sync. And the **accumulators are
  partitioned**: a size-N vector never resized, worker `w` touches only
  `locals[w]`, a **stolen task uses the *stealing* worker's accumulator** (via the
  `worker_id` arg), so it's **one writer per table for the whole phase** — never
  two threads, never even alternation. The **merge runs on the dispatcher thread**
  — not a pool worker — and sees every write through that one acquire edge; no
  merge-side lock."
- "**Honest scaling:** it'll be **sub-linear, and I'll measure it** — I don't
  claim near-linear and I won't quote a multiplier. Asymmetric cores (5 P + 6 E)
  and aggregation being **memory/probe-bound** (ADR 0008) flatten the curve well
  below 11×. The 1→N curve **isn't written yet** — it'll go in `BENCHMARKS.md`
  once the path is wired and measured; I won't point at a curve that doesn't
  exist."
- "**Determinism:** same groups as a **set** (order differs — `GROUP BY` is
  unordered). **Integer** SUM/COUNT/MIN/MAX are **bit-identical** to serial — and
  to nail the obvious objection, my integer SUM uses **wrapping two's-complement
  add**, but addition **mod 2⁶⁴ is associative and commutative**, so cross-thread
  reorder can't change the bits *even on overflow*; MIN/MAX are order-independent.
  I test that. **Double** SUM/AVG can differ in the **last ULP** because IEEE add
  is **non-associative** across thread order — I state that plainly, compare
  within epsilon, and use **no Kahan**. **Integer AVG is excluded** from
  bit-identical because it outputs double and an int64 sum > 2⁵³ loses bits on the
  double conversion (ADR 0011)."
- "**Scope honesty:** parallel aggregation is built and tested but **not yet wired
  into the executor**; filter/project would be embarrassingly parallel through the
  same `ParallelFor` but **aren't built yet**; **parallel join is P9** —
  thread-local/partitioned build + merge, read-only parallel probe —
  **foreshadowed, not built**."
