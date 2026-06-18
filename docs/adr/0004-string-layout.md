# 0004. String layout: the 16-byte German/Umbra string and an arena StringHeap (and why strings blunt SIMD)

## Status

Accepted.

## Context

P1 is the columnar data plane. For fixed-width types (`int32`, `double`, dates — which are integer-encoded and fixed-width) a `Vector` is a flat, contiguous array of POD values — exactly what SIMD wants: fixed stride, no indirection, stream the lanes. VARCHAR breaks all of that. A string is variable length and its bytes have to live *somewhere*; the obvious encodings are all bad for an analytical engine:

- A `Vector` of `std::string` pays a heap allocation *per value* once the string exceeds the small-string buffer, and carries a fixed per-value object footprint. On our pinned toolchain (libc++), `sizeof(std::string)` is **24 bytes** (it is 32 on libstdc++); those bytes are a *union* of the `{ptr, size, capacity}` control words and the SSO buffer — overlaid in the same 24 bytes, **not additive**. The problem is not the 24 bytes per se; it is that any string longer than the SSO buffer triggers a per-value heap allocation and scatters the actual bytes across the heap. Every comparison or hash then becomes a pointer chase into cold memory. In a columnar engine that processes batches of 2048 values per operator, this is death by allocation and cache miss.
- A `Vector` of `const char*` + a separate length array is leaner but still forces a pointer dereference for *every* comparison, even when two strings obviously differ in their first byte or in length.

The dominant string operations in TPC-H-style workloads are **equality** on categoricals (`l_shipmode = 'AIR'`, `l_returnflag`, `n_name`), **hashing** of string keys (joins, `GROUP BY` on `c_name`/`n_name`), **prefix/LIKE** predicates (`p_name LIKE 'green%'`), and **lexicographic sort** (`ORDER BY` on a string column). The equality and hashing cases are overwhelmingly satisfiable by *rejecting* a candidate — most pairs are not equal, most prefixes differ — so we want the common case to never touch the heap. The sort case wants an ordering key that is comparable cheaply, ideally from the same inline prefix.

This is the problem the **German string** layout — Andy Pavlo's informal nickname for the 16-byte layout introduced by the Umbra system (Neumann and Freitag, *Umbra: A Disk-Based System with In-Memory Performance*, CIDR 2020) — was designed for. DuckDB's `string_t` uses the same 16-byte / 4-byte-prefix / 12-byte-inline shape. We adopt that shape as-built, with one deliberate divergence noted in the Decision.

## Decision

A VARCHAR value is a 16-byte trivially-copyable POD, `StringRef` (verified `static_assert`s in `include/strata/data/string_ref.hpp`: `sizeof == 16`, `alignof == 8`, `is_trivially_copyable`):

```cpp
class StringRef {                  // sizeof == 16, alignof == 8
  union U {
    struct Pointer {               // length > 12
      uint32_t    length;          // 4 bytes  — common initial sequence
      char        prefix[4];       // first 4 bytes, always inline
      const char* ptr;            // full bytes in the StringHeap
    } pointer;
    struct Inlined {               // length <= 12
      uint32_t length;             // 4 bytes  — common initial sequence
      char     inlined[12];        // bytes live HERE, no heap
    } inlined;
  } u_;
  bool IsInlined() const { return size() <= kInlineLength; }  // 12
};
```

- **Inlined (length ≤ 12).** The bytes live in `inlined[12]`. No heap allocation, no pointer, no dereference. The whole storage is **zero-filled on construction** (`memset`), so trailing bytes beyond `length` are deterministic; two equal-valued inlined `StringRef`s are therefore bitwise identical, and hashing/comparison over the inline region is well-defined.
- **Pointer (length > 12).** The first 4 bytes are copied into `pointer.prefix`, and `pointer.ptr` points at the full `length` bytes stored in a `StringHeap`. Because this arm is taken only when `length > 12`, the string always has at least 4 bytes, so the prefix is **always fully populated from the string — there is no short/padding case here.** The prefix is **redundant** with the heap bytes by design — that redundancy is the whole point.
- **Why 12 is the cutoff.** The pointer arm spends 4 (prefix) + 8 (64-bit `ptr`) = 12 bytes of the union regardless. Inlining up to 12 bytes is therefore free: it reuses storage we were going to spend on the pointer machinery anyway. There is no layout reason to inline fewer.
- **Why 16 bytes total.** The size is *forced* by the arithmetic: 4 (length) + 4 (prefix) + 8 (64-bit pointer) = 16, which also happens to pass in two 64-bit registers. DuckDB and Umbra both use exactly this. It is not an independently tuned "sweet spot"; widening the inline buffer would mean growing the struct past 16 bytes, which costs scan bandwidth on *every* value.

**Deliberate divergence from Umbra.** Strata's pointer arm stores a **plain `const char*` with no stolen bits and no storage-class tag.** Umbra steals 2 bits from the pointer to encode a storage class (persistent / transient / temporary). We do not need that: our arena gives every long string the same whole-heap lifetime, so there is no per-string storage class to track. This is a simplification we make consciously, not an accident of copying.

**Equality — short-circuit, cheapest test first (as built, `operator==`):**

1. Compare `length` (one 4-byte load). Unequal length ⇒ not equal.
2. Compare the 4-byte prefix (`memcmp` of 4 bytes — the inline word for short strings, `pointer.prefix` for long ones). Mismatch ⇒ not equal. Still no heap dereference.
3. If `length <= 4`, the prefix already covered every byte ⇒ equal, return early.
4. Otherwise `memcmp` the full `length` bytes — and only here, for long strings, do we touch `ptr`.

For the overwhelmingly common "these two strings differ" case, we answer in steps 1–2 from registers/L1, never dereferencing the heap. **Length-first is an equality-only optimization** — see the next paragraph for why it is wrong for ordering.

**Ordering — a different protocol (do NOT reuse the equality steps).** Lexicographic `ORDER BY` cannot use length first: `"AIR" < "AIRMAIL"` even though `"AIR"` is shorter, and `"B" > "AIRMAIL"` even though `"B"` is shorter — length is not a function of order. The correct ordering protocol on this layout is:

1. Compare the **4-byte prefix as a big-endian integer**: load both prefixes as `uint32`, **byte-swap on little-endian** (`std::byteswap` / `__builtin_bswap32`), then compare the integers. The byte-swap is what makes a single integer compare equal lexicographic byte order. **Both target platforms are little-endian** (Apple M3 Pro and the x86_64 AVX2 CI runner), so the byte-swap is **mandatory**, not optional — a naive `uint32` prefix compare gives the wrong order.
2. On a prefix tie, compare the full bytes (`memcmp` over `min(len_a, len_b)`).
3. Only if one string is a proper prefix of the other does **length** break the tie (shorter sorts first).

The inline 4-byte prefix thus doubles as a cheap ordering/radix-sort key: the byte-swapped prefix integer is a correct first-pass sort key, which is exactly how Umbra and DuckDB use it. (As built, P1 ships `operator==` only; the ordering operator is specified here so the sort path in a later phase is implemented against the correct, byte-swap protocol rather than the equality shortcut.)

**StringHeap is an arena (bump allocator)** (`include/strata/data/string_heap.hpp`, `src/data/string_heap.cpp`):

- A list of **non-relocating** blocks; default block = 4096 bytes (`kBlockSize`). Allocation bumps a pointer (`tail_`) within the current block; when the request exceeds the remaining space, a new block is chained.
- **Oversized strings** (larger than what fits in the current block) get a fresh block sized `max(n, kBlockSize)`, so the string is still contiguous. The as-built policy then makes that new block the **active bump target**, so any slack beyond the oversized string (`block - n`) is still usable by subsequent allocations; the previously-active block is left chained and is simply no longer bumped into. This favours implementation simplicity over squeezing the last bytes of the old block — an acceptable, bounded fragmentation cost tied directly to the non-relocation guarantee below.
- Committed bytes **never move.** We only ever append blocks; existing blocks (and therefore the bytes a `StringRef::ptr` points at) stay put for the heap's lifetime — no relocation, no invalidation. This is precisely what makes it safe to store a raw `const char*` in the POD.
- **Each VARCHAR `Vector` owns its `StringHeap`.** Lifetime is the Vector's; freeing the Vector frees the whole arena in O(blocks). The heap is move-only (copy is deleted) because the bytes it owns back live `StringRef`s in the Vector.

**Correctness note (union discipline and copy semantics).** Two distinct guarantees, kept separate because they rest on different rules:

1. **Bitwise copy of the 16 bytes is valid** because `StringRef` is **trivially copyable** — full stop. A `memcpy` / bitwise copy of the object is well-defined; that is what lets a `StringRef` live in a raw Vector value buffer and be copied without a copy constructor. Trivial copyability says nothing about which union member is "active"; a trivial copy copies all 16 bytes regardless.
2. **Reading union members is valid** because (a) we always read the member consistent with `length` (the manual discriminant — `IsInlined()` chooses `inlined` vs `pointer`), and (b) `length` itself is the **common initial sequence** of both arms (same type at the same offset), so reading `length` through *either* arm is well-defined regardless of which arm is active. We never read a member that was not the one written. Copying a *pointer* `StringRef` aliases the heap bytes (it is a **view**, not a deep copy); deep copies go through the destination Vector's StringHeap explicitly (`StringHeap::Add`).

## Alternatives Considered

- **`std::string` per value.** Rejected: any string longer than the SSO buffer triggers a per-value heap allocation, and the bytes scatter across the heap, obliterating cache locality on batch scan/compare/hash. (`sizeof` is 24 on libc++ / 32 on libstdc++, a union of control words and SSO buffer — the footprint is not the headline problem; the per-value allocation and pointer chasing are.) Correct for a general-purpose mutable string; wrong for batch columnar processing.
- **`const char*` + parallel length array, bytes in a heap.** Rejected: no inline-small optimization (every value dereferences) and no prefix, so *every* comparison and hash chases the pointer even when the strings differ in byte 0. The German-string layout strictly dominates it for the common short-string case at the same or smaller footprint.
- **Length + prefix but no inlining (always pointer).** Rejected: short strings (which dominate categorical columns like `l_shipmode`, `n_name`, status flags) would needlessly allocate heap bytes and dereference. Inlining is free given the 12 bytes are already reserved.
- **Larger inline buffer (e.g., a 24-byte StringRef).** Rejected: it would widen *every* value in the column to chase a longer inline cutoff, trading column scan bandwidth (more bytes per lane, fewer values per cache line) for fewer heap strings. 16 bytes falls out of the 4+4+8 arithmetic and fits two registers; DuckDB and Umbra both use exactly it. We do not deviate without measurement.
- **Reference-counted / individually-freeable string storage.** Rejected: per-string lifetime management reintroduces the per-value bookkeeping the arena exists to eliminate. Batch/columnar processing wants whole-batch lifetime, not per-value `free`.
- **Interning / dictionary encoding of strings.** Not rejected on merits — it is a complementary, higher-level encoding (and a natural future optimization for low-cardinality columns). It is out of scope for the P1 data-plane representation, which must represent *arbitrary* strings.

## Consequences

**Wins:**
- Short strings (≤ 12 bytes) cost **zero** heap allocations and **zero** dereferences — pure POD living in the Vector's value array.
- Equality and hashing reject most non-matches on `length` or the 4-byte prefix, from registers/L1, **without** touching the heap — the German-string insight.
- The inline 4-byte prefix doubles as a cheap **ordering/radix key** (byte-swapped) for the sort path, so even `ORDER BY` gets a dereference-free first pass.
- The arena turns N per-value heap allocations into **N cheap bump-pointer increments** (one per long string, O(1), no `malloc`) plus only **~N/(strings-per-block) actual block allocations** (the rare, expensive `new[]`), and one O(blocks) teardown. Excellent locality; bytes produced together sit together.
- Non-relocating blocks make `const char*` stable, so the POD can safely embed a raw pointer — no fat smart-pointer, no relocation hazard.
- `StringRef` is trivially copyable: a Vector of `StringRef` is a flat POD array, so the length+prefix words *can* be scanned across SIMD lanes (with the caveat below).

**Tradeoffs accepted:**
- **16 bytes per value even for a 1-character string** (vs an 8-byte bare pointer). Wider columns mean fewer values per cache line on scan. We pay this everywhere to make the short-string and reject-fast cases free.
- **The 4-byte prefix is redundant** with the heap bytes for long strings — duplicated storage, by design, in exchange for dereference-free rejection/ordering.
- **The arena cannot free an individual string.** Freeing is whole-heap, tied to the Vector's lifetime. Correct for batch/columnar processing, wrong for long-lived mutable storage; Strata is the former, so this is a feature, not a bug. The oversized-block policy also abandons the slack in a partially-filled block when an oversized string arrives — bounded, accepted fragmentation in exchange for simplicity and the non-relocation guarantee.
- **Union discipline and the equality-vs-ordering distinction are manual invariants.** Reading the member matching `IsInlined()`, and using length-first only for equality (never ordering), are programmer-enforced, not type-system-enforced. They are localized to the StringRef accessors/comparators and are the price of a 16-byte POD with no tag byte beyond `length`.

**The honesty centerpiece — strings blunt SIMD** (the concrete mechanism behind the "SIMD ceiling" in `docs/LIMITATIONS.md`):

- **Variable length defeats fixed-width lane processing.** SIMD wants a fixed stride per element. A string body has no fixed width, so the *bytes* of a column cannot be loaded into lanes the way `int32`s can.
- **String comparison is a byte loop / `memcmp`, not a single lane op.** Comparing two long strings is sequential byte scanning (or `memcmp`, itself a possibly-vectorized loop *within one comparison*) — not "compare 4 values in one instruction." We do not get one-instruction-per-element throughput across values.
- **The bytes sit behind a pointer ⇒ gather/indirection, not streaming contiguous loads.** For long strings the data is in the heap at a `ptr`; reaching it is a gather/indirect access, which SIMD hardware handles far worse than a contiguous streaming load. On our target (Apple M3 Pro, ARM NEON 128-bit, no AVX) there is **no wide native gather** to lean on; Highway compiles to AVX2 on the x86_64 CI runner, but the indirection problem is the same.

Net: **string-heavy operators see little or no SIMD speedup**, and we will **measure and report** that in P3 rather than hide it.

The 16-byte fixed `StringRef` *does* recover **some** SIMD-friendliness, stated precisely and without overclaiming. Because the Vector is a flat array of 16-byte PODs, the `length` and 4-byte `prefix` words sit at fixed offsets and *can* be loaded and compared across lanes to accelerate the **reject phase** of equality/filtering (and the first-pass prefix compare of sorting). **But the stride is 16 bytes:** consecutive `length` fields are 16 bytes apart (interleaved with the prefix/pointer/inline tail), so loading many `length`s or `prefix`es across lanes is a **strided / deinterleave load, not a flat contiguous load** — and on NEON, with no gather, that means strided loads plus shuffles. The reject-phase win is therefore **real but bounded by deinterleave/shuffle cost**, and it applies only to the metadata words — it does **not** vectorize the variable-length `memcmp` tail and does **not** turn full string comparison into a per-lane op. We will **measure** this win in P3, not assume it.

## How to defend this at a whiteboard

- "A VARCHAR is a 16-byte POD: 4-byte length + a 12-byte union that is either 12 inline bytes or {4-byte prefix, 8-byte pointer}. It's the Umbra layout — 'German string' is Andy Pavlo's nickname for it; Umbra is Neumann & Freitag, CIDR 2020 — and it's the same shape as DuckDB's `string_t`."
- "12 is the cutoff because the pointer arm already burns 4 + 8 = 12 bytes; inlining up to 12 is free. 16 total falls out of 4 + 4 + 8 and fits two registers — it's not a tuned magic number."
- "We diverge from Umbra deliberately: plain `const char*`, no stolen pointer bits, no storage-class tag. Our arena gives every long string the same whole-heap lifetime, so there's no per-string class to encode."
- "Equality short-circuits: length, then 4-byte prefix, then `memcmp`; if length ≤ 4 the prefix already covered every byte. Most non-matches die in the first two steps with no heap dereference."
- "Ordering is a *different* protocol — length-first is wrong for sort (`'AIR' < 'AIRMAIL'`, `'B' > 'AIRMAIL'`). You compare the 4-byte prefix as a **byte-swapped (big-endian) integer** first, then the bytes, and use length only to break a proper-prefix tie. Both my platforms are little-endian, so the byte-swap is mandatory; that byte-swapped prefix is also a radix-sort key."
- "Bytes live in a per-Vector arena — a bump allocator of non-relocating blocks. Each long string is one O(1) pointer bump; only every-so-often do we actually `malloc` a 4 KiB block. Teardown is O(blocks); pointers stay valid because blocks never move, which is why I can store a raw `const char*` in the POD."
- "Why not `std::string`? `sizeof` is 24 on libc++ — a union of the control words and the SSO buffer, not additive — but the real killer is per-value heap allocation for anything past the SSO buffer, plus pointer chasing. Fatal for a columnar engine doing 2048-value batches."
- "Honest cost: 16 bytes even for tiny strings, the prefix is redundant for long strings, and the arena can't free one string — whole-heap lifetime. Correct for batch processing, wrong for long-lived mutable storage."
- "Strings blunt SIMD: variable length defeats fixed lanes, comparison is a byte loop / `memcmp`, and long bytes sit behind a pointer (gather, not streaming load) — and NEON has no native gather. So string-heavy operators get little SIMD speedup; I measure it in P3, I don't hide it."
- "The fixed 16-byte StringRef recovers a *modest* reject-phase win on the length/prefix words — but those live at a 16-byte stride, so it's a strided/deinterleave load plus shuffles, not a clean contiguous lane load, and it doesn't touch the `memcmp` tail. Real but bounded; I'll measure, not assume."
- "Correctness, two separate guarantees: (1) the 16-byte bitwise copy is valid because the type is trivially copyable — that's all that gives me; (2) reading a union member is valid because I only read the member matching `length`, and `length` is the union's common initial sequence so it's readable through either arm. A pointer `StringRef` copy is a view that aliases the heap bytes; deep copies route through the destination's StringHeap."
