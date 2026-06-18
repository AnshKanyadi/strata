# 0005. Vector memory ownership (owning, 64-byte-aligned buffers) and vector kinds (flat + constant)

## Status

Accepted.

## Context

P1 is the columnar data plane: every operator in Strata reads and writes `Vector`s, which are the columns of a `DataChunk` (a batch of up to `VECTOR_SIZE = 2048` values). Two decisions about `Vector` are load-bearing for the entire engine and hard to change later, so we fix them here:

1. **Who owns a vector's storage**, and what its lifetime/copy semantics are.
2. **What physical layouts ("kinds")** a vector can take, since downstream operators must handle each kind.

These choices propagate everywhere. The kernels in P3 load from a vector's buffer with SIMD; the expression evaluator and every operator branch on vector kind; lifetime mistakes here are use-after-free bugs that surface as data corruption under load, not clean crashes. We want the simplest model that is *obviously* correct first, with a clearly marked boundary where we will later trade simplicity for the zero-copy tricks DuckDB uses.

The reference point is DuckDB (and behind it MonetDB/X100 — Boncz, Zukowski, Nes, CIDR 2005 — which established the vectorized batch-at-a-time model Strata follows). DuckDB's `Vector` does **not** own a plain buffer: it holds a reference-counted shared buffer (`buffer_ptr<VectorBuffer>`, a `shared_ptr`-based handle), caches a raw `data_ptr_t` into that buffer for fast access, and keeps a *separate* auxiliary `VectorBuffer` for out-of-line data (string bytes, list/struct children). That refcounted indirection is what lets DuckDB **slice** a vector and build **dictionary** vectors with **zero copy** — multiple vectors share one underlying buffer and differ only by a selection vector. P1 deliberately does not start there.

## Decision

**A `Vector` owns its storage.** Concretely a `Vector` holds, by value/RAII:

- A **data buffer**: raw bytes allocated **64-byte-aligned** via aligned `operator new(size, std::align_val_t{64})`, then **explicitly zero-filled** (`std::memset`) as a separate step — `operator new` itself returns *uninitialized* storage and does no zeroing. The buffer is freed with the matching **sized, aligned** `operator delete(ptr, size, std::align_val_t{64})`. Zero-fill is for reproducible debugging and defined bit-patterns under a debugger, not for read correctness (see the lifetime note: correctness rests on write-before-read, not on the zero-fill).
- Its **`Validity`** (the null bitmask for this vector).
- For `VARCHAR`, its **`StringHeap`** (the backing store for the out-of-line bytes of long strings). Strata uses the 16-byte "German string" / Umbra-style layout — a length plus an inline prefix, with short strings stored fully inline and long strings carrying a pointer into the heap (the Umbra system, Neumann & Freitag; DuckDB's `string_t` uses the same idea). The dedicated ADR for that layout is forthcoming; until it lands this ADR describes the heap's relationship to the vector inline rather than cross-referencing a record that does not yet exist.

A `Vector` is **move-only**: the copy constructor and copy assignment are deleted; move transfers buffer ownership. This makes accidental deep copies a compile error rather than a silent performance cliff, and gives each buffer exactly one owner with a deterministic free.

**Two vector kinds are implemented:**

- **FLAT** — a contiguous array of `count` values in the buffer, `value[i]` at byte offset `i * sizeof(T)`. This is the default. For **fixed-width** element types (`int32_t`, `int64_t`, `double`), it is the SIMD fast path: a kernel loads consecutive lanes straight from the buffer. For `VARCHAR`, FLAT means a contiguous array of 16-byte German-string structs; the SIMD benefit is on the fixed-width columns, **not** on string payloads (those are variable-length and out-of-line, so loading the structs "as lanes" buys no speedup on the string *contents*).
- **CONSTANT** — a single logical value physically stored at **slot 0** that represents *all* logical rows. Validity is a **single bit** (the whole vector is null, or none of it is). Readers of a constant vector read slot 0 for every logical row `i`.

**DICTIONARY vectors are deliberately deferred** to a later stretch — for scope control, and because the dictionary variants worth building need machinery P1 does not have yet. To be precise about *why*, since this is load-bearing:

- A dictionary vector over an **owned** child (the vector owns its child buffer outright, with a selection vector indexing into it) **is** implementable in the owning model today, single-owner and RAII, with no extra copy. So "owning buffers forbid dictionary" would be false.
- What the owning model forecloses is the *high-value* dictionary use cases: **zero-copy sharing** of one child across multiple vectors, and **zero-copy slice-to-dictionary** (producing a dictionary view over an existing vector's buffer without copying it). Those are precisely what reference-counted buffers buy and what motivate dictionary in the first place.

So we defer dictionary as a unit: a half-built dictionary-over-owned-child would carry the operator-dispatch cost of a third kind while delivering little of dictionary's payoff, which lands when shared buffers land.

**Typed access** is via `reinterpret_cast<T*>(buffer + offset)` over the byte buffer, for **implicit-lifetime** element types (scalar types like `int32_t`/`int64_t`/`double`, and trivially-copyable aggregates with trivial destructors, such as the inline German-string struct). This is well-defined under C++20 — see the lifetime/aliasing note below. The 64-byte alignment guarantees the resulting `T*` is suitably aligned for both scalar access and aligned SIMD loads.

## Alternatives Considered

**Reference-counted / shared buffers (the DuckDB model).** A `Vector` holds a `shared_ptr`-like handle to a `VectorBuffer`; slicing and dictionary encoding share the buffer and never copy the payload.
*Why rejected (for now):* it front-loads the hardest correctness reasoning in the engine — aliasing, atomic refcount traffic, who-may-mutate-a-shared-buffer, copy-on-write — before we have a single working operator. We want P1 to be auditable by reading one owner per buffer. We accept that operations DuckDB does zero-copy (slice, dictionary-encode) cost us a copy. This is a **revisitable optimization boundary**, not a permanent stance: when the profiler shows slicing/dictionary copies dominating, we revisit shared buffers as a focused change, with the operator interfaces (which already branch on kind) mostly unchanged.

**Copyable vectors (implicit deep copy).** Allow `Vector` copy construction, copying the buffer.
*Why rejected:* it makes the expensive thing (a 2048-wide buffer copy) the *easy, invisible* thing. A stray pass-by-value in an operator becomes a per-batch deep copy that no reviewer notices. Move-only forces the cost to be written down (`std::move`, or an explicit `.Copy()` when a copy is truly intended).

**Non-owning vectors (views into an arena/buffer pool) as the P1 default.** Every vector is a `{ptr, count}` view; some other component owns the bytes.
*Why rejected:* it just relocates the lifetime problem to "who owns the arena and when is it reset," which is the same shared-buffer reasoning without the safety of refcounting. We may introduce an arena later for the buffer *allocator*, but ownership in P1 stays with the `Vector`.

**Skip CONSTANT; everything is FLAT.** Materialize the literal `5` in `x + 5` into 2048 copies of `5`.
*Why rejected:* it discards a free win. A literal, a constant-folded subexpression, or a column the optimizer proved constant should be stored once and never materialized N-wide; CONSTANT lets `add(column, scalar)` broadcast the single scalar into the SIMD lanes instead of loading 2048 copies of the literal, and lets a constant input or result propagate through an operator without ever touching N slots.

**`std::aligned_storage` / hand-rolled union storage.** *Why rejected:* `std::aligned_storage` is deprecated in C++23, and raw aligned `operator new` + `reinterpret_cast` is both simpler and exactly what the standard now blesses for implicit-lifetime types (below).

## 64-byte alignment rationale

Strata compiles SIMD kernels for exactly **two** targets, with these vector widths:

- **NEON** (Apple M3 Pro, arm64): 128-bit / **16-byte** vectors.
- **AVX2** (x86_64 CI runner, via Highway): 256-bit / **32-byte** vectors.

So the **maximum alignment any compiled target requires is 32 bytes** (AVX2). We nevertheless align to **64 bytes**, and the honest justification is the **cache line**, not a SIMD requirement: 64 bytes is the cache-line size on these machines, so a vector buffer starts on a cache-line boundary and the P3 kernels never pay for a **split / unaligned cache-line load** at the start of a column. 64 bytes also leaves headroom for a hypothetical future **AVX-512** build (512-bit / 64-byte vectors) — but Strata does **not** target AVX-512 today (the M3 has no AVX at all; the x86 runner is AVX2), so we do not claim it as a current driver of the alignment.

The general rule for choosing one number across power-of-two alignment requirements is the **maximum** of them (which for powers of two equals their LCM): max(16, 32) = 32 for what we compile, rounded up to 64 for the cache line. A single allocation policy then gives **aligned SIMD loads on every target we build** plus cache-line-aligned starts. Aligned loads are also the only loads defined for the alignment-requiring intrinsics; Highway abstracts the intrinsic selection, but the underlying alignment contract is satisfied by construction.

## Aliasing and lifetime correctness note

The concern: we allocate raw bytes (`operator new`) and then access them as `int32_t`/`double`/the German-string struct via `reinterpret_cast`, without ever running a `T` constructor. Is reading/writing through that `T*` defined?

For Strata's element types — all **implicit-lifetime** types (scalar types, and trivially-copyable aggregates with trivial destructors; the German-string struct qualifies as such an aggregate) — **yes, under C++20**. C++20 introduced **implicit object creation (IOC)**: an operation that provides suitable storage (notably `operator new` returning suitably-sized, suitably-aligned storage) implicitly creates objects of implicit-lifetime type in that storage **when doing so gives the program defined behavior**. The precise mechanism matters:

- The **storage-providing operation** (`operator new`) is what implicitly creates the objects; the set created is "whatever set makes the program have defined behavior."
- A `reinterpret_cast` does **not** itself create an object, and does not on its own "produce a pointer to" the implicitly-created object — that under-specification is exactly what motivated `std::start_lifetime_as`. What carries the weight is the **store through the typed lvalue**: writing `*reinterpret_cast<int32_t*>(buffer + i*4) = v;` is an operation that *requires* an `int32_t` to live at that address to be defined, which is what forces an `int32_t` to have been implicitly created there. A subsequent read then returns that value.

Two conditions make this airtight, and we hold both:

1. **Never read a slot before it is written.** This is the load-bearing defense. A typed read of a slot whose `T` object was never assigned reads an **indeterminate value**, which for non-byte types is itself **undefined behavior** — and zero-filling the raw buffer does **not** fix this: a `memset` writes bytes, not an `int32_t` lvalue, so it does not give a later typed read a defined value under the abstract machine. Zero-fill only makes the *observed bit pattern* deterministic for debugging/repro; it does **not** convert the UB of reading a never-written slot into defined behavior. Correctness therefore rests **solely** on the discipline that every operator writes a slot (or marks it null in `Validity`) before any consumer reads it.
2. **Alignment is satisfied.** Forming and dereferencing a `T*` requires the address be aligned for `T`; 64-byte buffer alignment makes every slot offset `i * sizeof(T)` aligned for our types, so both scalar dereference and aligned SIMD loads are defined.

Honesty about the pedantic tool: the maximally-explicit, can't-be-misread spelling is **`std::start_lifetime_as<T>` / `std::start_lifetime_as_array<T>`** (C++23), which names the lifetime-start without a store. We do **not** rely on it: because we always store before we load, the store is the operation that gives defined behavior and thus forces the object into existence under IOC. We cite `start_lifetime_as` as the canonical reference for *why* this is legal, not as something the code calls. We also do **not** claim this generalizes to non-implicit-lifetime types (e.g. a trivially-copyable class with a *non-trivial* destructor is not implicit-lifetime) — Strata's vector element types are all implicit-lifetime by design.

## Consequences

**Wins**
- **Obvious lifetimes.** One owner per buffer, RAII free, move-only. No refcounts, no shared-mutation reasoning, no aliasing-across-vectors. P1 is auditable by reading a single class.
- **Defined, fast loads.** 64-byte alignment gives aligned SIMD loads on NEON and AVX2 and cache-line-aligned buffer starts for the P3 kernels, from one allocation policy.
- **No accidental deep copies.** Move-only turns a 2048-wide buffer copy into a compile error unless explicitly `.Copy()`-ed.
- **Constant fast path.** CONSTANT removes N-wide materialization of literals and proven-constant columns and enables scalar-broadcast kernels.

**Tradeoffs accepted**
- **Extra copies vs DuckDB's shared buffers.** Slicing and dictionary-encoding, which DuckDB does zero-copy, cost us a buffer copy. We accept this for P1 and have marked shared buffers as the explicit revisit point.
- **No DICTIONARY kind yet.** A dictionary-over-owned-child is implementable today, but the valuable variants (zero-copy sharing, zero-copy slice-to-dictionary) want shared buffers, so we defer the whole kind. We forgo dictionary compression and sharing until shared buffers land.
- **Alignment waste on short vectors.** Rounding every allocation up to a 64-byte boundary wastes up to 63 bytes per buffer; negligible for full 2048-wide vectors, proportionally larger for short ones. Accepted as the cost of one uniform alignment policy.
- **Kind-check branch in generic accessors.** A generic reader must check FLAT vs CONSTANT, adding a branch (and discipline to canonicalize correctly). We mitigate with a dedicated **flat fast path** so the common hot case pays no dispatch beyond a predictable branch.
- **Move-only ergonomics.** Callers must `std::move` vectors and cannot casually copy them; intentional friction, but friction.

## How to defend this at a whiteboard

- "A Vector **owns** its bytes — 64-byte-aligned, RAII, move-only. One owner, deterministic free, no refcount reasoning. DuckDB uses **reference-counted shared buffers** (`buffer_ptr<VectorBuffer>` plus a separate auxiliary buffer for strings) so it can slice and build dictionaries **zero-copy**; I traded that for obvious correctness in P1 and I know exactly where the copies are."
- "Why 64? My compiled SIMD targets are NEON (16B) and AVX2 (32B), so the SIMD max I actually need is 32. I round to 64 for the **cache line** — kernels never eat a split cache-line load at the buffer start — with headroom for a hypothetical AVX-512 build I don't compile today. I won't claim AVX-512 as a driver; it isn't one."
- "Two kinds: **FLAT** (contiguous; the SIMD fast path for fixed-width int/double, while for VARCHAR it's just contiguous 16-byte string structs with no SIMD win on the payloads) and **CONSTANT** (one value at slot 0 for all rows, one validity bit). CONSTANT is why `x + 5` doesn't materialize 2048 fives — the scalar becomes a broadcast register — and why a constant input or result can flow through an operator untouched."
- "I `reinterpret_cast` raw `operator new` storage to typed pointers. Legal under **C++20 implicit object creation** because the element types are **implicit-lifetime** and I **always write a slot before reading it** — the *store* through the typed lvalue is what forces the object into existence, not the cast. zeroing the buffer is for debug repro only; it does **not** make reading a never-written slot defined, so write-before-read carries the correctness. The unambiguous spelling is `std::start_lifetime_as` (C++23); I don't need to call it. Alignment is the other half — a `T*` must be aligned to be dereferenceable, and 64-byte buffers guarantee that."
- "**DICTIONARY is deferred** for scope. A dictionary-over-owned-child would actually fit the owning model — what owning *forecloses* is zero-copy *sharing* of a child across vectors and zero-copy slice-to-dictionary, which is the payoff that justifies dictionary. So it lands when shared buffers land. The operator interfaces already branch on kind, so adding a third kind is additive."
- "The honest cost: extra copies on slice/dictionary, a little alignment waste, a kind-check branch, and move-only friction. All four are stated, bounded, and the shared-buffer revisit is the lever I'd pull when the profiler says copies dominate."
