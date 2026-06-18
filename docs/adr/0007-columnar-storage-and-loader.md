# 0007. In-memory columnar storage (ColumnarTable) and the delimited loader

## Status

Accepted

## Context

Strata is a push-based vectorized engine: operators consume and produce `DataChunk`s of up to `kVectorSize = 2048` rows, columnar within a chunk (one `Vector` per column, per ADR 0005). Before any operator can run, base table data must live somewhere in a shape the scan can hand to the pipeline, and external delimited text (TPC-H `.tbl`; comma-delimited files with no quoting/escaping) must be parsed into that shape.

Two questions: (1) what is the in-memory representation of a materialized base table, and (2) how do we ingest delimited text into it. Both have to serve P2's goals — correctness, validation against DuckDB, simplicity — while not painting P8 (morsel-driven parallelism) into a corner.

The in-memory layout also has to honor ADR 0006's borrow model: a scan must be able to expose stored data to the pipeline without copying, which constrains how the table owns and lays out its chunks.

## Decision

### ColumnarTable

A `ColumnarTable` is:

- a **`Schema`**: an ordered list of named, typed columns over the fixed type set **BOOL, INT32, INT64, DOUBLE, DATE, VARCHAR**;
- a **`std::vector<DataChunk>`**: a flat sequence of execution-sized chunks, each holding up to `kVectorSize = 2048` rows across all columns, columnar within the chunk;
- `row_count` = the sum of the per-chunk row counts.

The table **owns** its chunks. `DataChunk` is move-only (it owns `Vector`s, which own aligned buffers, `Validity`, and `StringHeap` per ADR 0005), so the chunks live in a `std::vector<DataChunk>` and the table is itself move-only. There is no shared ownership and no reference counting on storage.

**Build-then-freeze; pointer stability.** The chunk vector is built entirely at load time and **frozen before any scan runs** — there is no `push_back` into a table that is concurrently or subsequently being scanned. This matters because a growing `std::vector` can reallocate and relocate its elements: if scans borrowed `const DataChunk*` into a still-growing vector, those pointers could dangle. By construction, scans only ever borrow into a finalized, non-mutating vector, so element addresses are stable for the table's lifetime. `DataChunk` is `nothrow`-move-constructible, so the vector *moves* its elements (rather than attempting to copy this move-only type) when it grows during load.

### Scan as a zero-copy walk

A **Scan source** walks the stored chunks and, for each, hands the pipeline a `const DataChunk*` pointing directly at the table's owned chunk — **no copy**. Storing data already in execution-sized chunks is exactly what makes this a pointer walk rather than a re-batching step: there is no slicing, no gather, no allocation on the scan hot path.

**The borrow contract (both halves).** Because this is a push model, the scan calls into downstream operators synchronously, and the borrowed `const DataChunk*` is valid only **for the duration of that synchronous downstream `consume()` call chain** — not vaguely "for the duration of the push." Soundness requires *two* conditions, not one:

1. **Necessary backstop:** the table outlives the query, so the underlying chunk storage cannot disappear mid-borrow.
2. **The actual hazard this design creates:** any downstream operator that needs data *beyond* the current `consume()` call — a hash-join build side, an aggregate accumulating state, any pipeline breaker — **must materialize/copy** the data it keeps. It may **not** retain the borrowed pointer. The table outliving the query is *necessary but not sufficient*; the no-retain rule on consumers is the other half of the contract.

### Why execution-sized (≤2048-row) chunks as the storage unit

Storing the table as a flat list of 2048-row chunks buys two things:

1. **Zero-copy scan.** Storage chunks *are* execution chunks, so the scan hands them straight to the pipeline.
2. **Pre-shaped for future morsel-driven parallelism (P8).** The chunked layout is *designed so that* a future scheduler can assign work as index ranges over `chunks` without re-chunking. P8 does not exist yet; the storage is merely amenable to it (see below).

**Honest contrast with DuckDB.** DuckDB stores data in larger **row groups** of **122,880 rows** (= 60 × its 2048 vector size); the row group is its unit of compression, of parallel scan assignment, and of min/max **zone maps** used to skip work, and each row group is then *subdivided* into 2048-row vectors for execution. (Row-group sizing across columnar systems is not a single number: Parquet sizes row groups by *bytes*, not a fixed row count — commonly ~128 MB, often hundreds of thousands to ~1M rows; Arrow record batches vary; other engines pick yet other granularities, generally driven by compression-block / I/O / metadata-amortization concerns rather than by being an integer multiple of a vector size. The 122,880 figure is specifically DuckDB's.) Strata instead uses a flat list of 2048-row chunks. This is simpler for P2 and is friendly to coarse-grained parallelism, but it is a deliberate downgrade relative to a row-group design:

- **More per-chunk storage metadata.** One `DataChunk` (plus its per-column `Vector` + `Validity` + `StringHeap` metadata) per 2048 rows. Compared specifically to **DuckDB's 122,880-row row group**, that is **~60× more storage-side chunk wrappers** over the same data — the factor is just `row_group_size / 2048`, so against a different row-group size it would be a different factor (e.g. ~49× at 100K rows, ~488× at 1M). The cost is in *persistent storage-side* metadata count, not in payload bytes — and note a row-group design *also* materializes per-2048-vector objects at execution time, so the saving is in the storage wrapper count, not in execution-time vector objects.
- **No row-group-level compression / encoding.** Values are stored raw (see below).
- **No zone maps.** Without a row-group min/max summary, there is no scan-skipping; a predicate over a sorted-ish column still touches every chunk.

**Row groups are the expected later refinement**: wrap N chunks into a row group carrying min/max zone maps and (eventually) per-column compression, keeping the 2048-row vector as the execution unit underneath. The chunked storage layout makes that an additive change, not a rewrite.

**On P8 morsels (forward-looking, not as-built).** A morsel in Leis et al. (SIGMOD 2014) is a fixed-size slice of input that a worker dispatches against shared global pipeline state; crucially, **morsel size is decoupled from the vector/execution size** so the scheduler can tune granularity and balance load (Leis uses ~100K-row morsels). A single 2048-row chunk is *far too small* to be a good morsel. So the intended design is: a future morsel will be a **run of N chunks** claimed **atomically** (e.g. an atomic fetch-add over a shared chunk-index cursor). Lock-freedom comes from *that atomic claim mechanism*, not from chunk boundaries "being natural." What the chunked layout actually provides is that storage is **pre-partitioned at chunk granularity**, so claiming a run of chunks needs no re-chunking; the morsel size (the run length) is independent of the 2048 vector size and will be tuned larger for load balancing.

### The loader

The loader is a **single-character-delimited line parser** over a `std::istream`, with a thin wrapper that opens an `std::ifstream` from a file path. It returns `Result<ColumnarTable>` — all failures (I/O error, parse failure, wrong field count) are reported at the **setup boundary** via `std::expected`, consistent with ADR 0002 (exceptions/errors at the query/setup boundary, never in the per-batch hot path).

Options:

- **Configurable delimiter**: `'|'` for TPC-H `.tbl`; `','` when the input happens to use a comma (this is "delimiter = comma," not a CSV-format claim — see Tradeoffs).
- **Trailing-delimiter handling**: TPC-H `.tbl` terminates every line with `'|'`; the loader can drop the empty trailing field so field count matches the schema.
- **Optional header row**: skipped when present.
- **Empty-field ⇒ NULL** rule, with a **configurable null token** (default: the empty field).

The caller **supplies the `Schema`** — there is no schema inference. The loader fills chunks up to `kVectorSize`, then starts a new chunk; the final partial chunk is kept.

**Typed parsing.**

- **INT32 / INT64 / DOUBLE** via `std::from_chars`.
- **DATE** parsed from `'YYYY-MM-DD'` into an `int32` count of **days since 1970-01-01** using Howard Hinnant's `days_from_civil` algorithm.
- **BOOL** from the tokens `true`/`false`, `1`/`0`, `t`/`f`.
- **VARCHAR** copied into the destination column's `StringHeap` (ADR 0004 StringRef: inline ≤12B, else 4B prefix + arena pointer).

### Why `from_chars` (not `strtod` / `stoi` / `istream >>`)

`std::from_chars` is the right primitive for parsing fixed-format data:

- **Locale-independent by specification.** It always uses `'.'` as the decimal point and never groups digits. By contrast, `strtod` follows the **C** `LC_NUMERIC` locale, and `istream >>` follows the stream's **imbued** `std::locale` (the default is the classic `"C"` locale — dot decimal — unless the stream is imbued or the global locale is synced/changed). Either can be switched to a comma-decimal locale (e.g. the global locale set from the environment), at which point every `DOUBLE` silently misparses — a latent, environment-dependent data-corruption bug. `from_chars` removes that failure mode entirely.
- **No heap allocation.** Unlike `std::stoi`/`std::stod` (which construct a `std::string` and can throw), `from_chars` parses in place out of the field's `char` range.
- **Rejects trailing garbage.** It returns a pointer to the first unconsumed character; the loader requires that pointer to reach the field's end. So `"12x"` is a **parse error**, not a silent `12`. `strtod`/`atoi` would accept the `12` and drop the `x`.

**Toolchain availability.** The integer `from_chars` overloads have been broadly available for years (GCC 8 / early libc++); the floating-point overloads (P0067R5) landed later — completed in **GCC 11** for libstdc++ (which is why gcc-14, our secondary verified target, has them), and added to libc++ comparatively recently as well. The **primary toolchain actually relied on is Homebrew LLVM clang 22 / libc++**, with gcc-14 / libstdc++ as the secondary verified target; `from_chars` for **both** integer and floating-point is a recorded verified environment fact on both.

### Why empty-field ⇒ NULL (and why it is a policy, not a law)

In delimited text there is no in-band way to distinguish "empty string" from "absent value"; a column reaching the loader as zero bytes has to mean *something*. Mapping it to **NULL** is the most common and least surprising convention and matches TPC-H's treatment of absent fields. This is an explicit **policy choice**, not a universal truth: some producers use a sentinel (e.g. PostgreSQL `\N`) and reserve the empty field for the empty string. Hence the null token is **configurable** — set it to `\N` and an empty field becomes a genuine empty `VARCHAR`.

### Why DATE as `int32` days-since-epoch via `days_from_civil`

Dates are stored as a single `int32` day count, computed by Hinnant's `days_from_civil(y, m, d)` (`days_from_civil(1970, 1, 1) == 0`, the Unix epoch):

- **O(1), branch-light, allocation-free.** A handful of integer ops — no `<chrono>` `parse`/`format`, no `std::tm`, no `mktime`/`timegm` (which are locale- and timezone-sensitive and far slower).
- **Cheap downstream arithmetic.** Date comparisons and range predicates (ubiquitous in TPC-H, e.g. `l_shipdate` filters) become plain `int32` integer compares.

**Honest scope.** The algorithm is **proleptic Gregorian** (it extends the Gregorian calendar backward without reform discontinuities) and is a **pure arithmetic formula**: `days_from_civil` performs **zero validation** — it maps any `(y, m, d)` integers to a day number, including nonsense like `m = 13` or `d = 40` (e.g. `days_from_civil(2021, 2, 30)` returns `18688`, no error). All calendar correctness therefore rests on the loader's checks, which are currently shallow: the loader enforces the `YYYY-MM-DD` *shape* (three numeric fields in the expected positions) but does **not** check that the day-of-month actually exists for the given month — so `2021-02-30` is accepted. Inputs are assumed well-formed, which holds for TPC-H-generated dates. Strict calendar validation is out of scope for P2.

## Alternatives Considered

- **Store base tables as one large columnar block per column (no chunking), slice into vectors at scan time.** Rejected: the scan would have to construct 2048-row views or copies on the fly, complicating the zero-copy borrow model (ADR 0006) and adding per-batch work. Pre-chunking moves that cost to load time, once.

- **Adopt full row groups now (DuckDB-style 122,880-row groups subdivided into vectors, with zone maps + compression).** Rejected for P2 as premature: it adds a second layer of structure, a compression/encoding subsystem, and zone-map maintenance before any of it is needed for correctness or for beating the baseline on the target queries. It is recorded as the **expected next refinement**, and the chunked layout is chosen to make it additive.

- **`strtod` / `stoi` / `istream >> x` for parsing.** Rejected: locale-dependence risks silent numeric corruption (C `LC_NUMERIC` for `strtod`, the imbued stream locale for `istream >>`), the `std::sto*` family allocates and throws, and all of them silently accept trailing garbage. `from_chars` fixes all three.

- **A full RFC-4180 CSV parser (quoting, escapes, embedded delimiters/newlines).** Rejected for P2: see Tradeoffs. TPC-H `.tbl` contains none of those constructs, so the machinery would be cost without benefit for the primary workload. Recorded as a known limitation.

- **`<chrono>` `std::chrono::parse` / `std::tm` + `mktime` for dates.** Rejected: heavier, allocation- and locale/timezone-sensitive, and slower than a direct integer formula, for no gain over a stored `int32` day count.

- **mmap / zero-copy ingest of the source file.** Rejected for P2: the loader copies parsed values into owned columns. mmap-based ingest is a real later optimization but is **not** cleanly orthogonal to the string model — see Tradeoffs.

- **Schema inference from the data.** Rejected: inference is heuristic and error-prone; for a benchmark engine validated against DuckDB, an explicit caller-supplied `Schema` is more honest and reproducible.

## Consequences

**Wins.**

- Scan is a true zero-copy pointer walk over owned, frozen chunks; no re-batching between storage and execution.
- Storage is pre-partitioned at chunk granularity, so future morsel-driven parallelism (P8) can claim runs of chunks without re-chunking.
- Numeric parsing is locale-safe, allocation-free, and rejects trailing garbage (`"12x"` fails).
- Dates are a single `int32`, making date predicates plain integer compares.
- All ingest errors surface as `Result<ColumnarTable>` at the setup boundary; the hot path stays exception-free.

**Tradeoffs (stated honestly).**

- **Per-chunk storage metadata vs. row groups.** ~60× more storage-side chunk wrappers than DuckDB's 122,880-row row group (the factor is just `row_group_size / 2048`); the cost is persistent storage metadata count, not payload bytes, and execution still vectorizes at 2048 in both designs.
- **No compression / encoding.** Values are stored raw; no dictionary (deferred per ADR 0005), no RLE, no bit-packing.
- **No zone maps / scan skipping.** Without row-group min/max summaries, predicates scan every chunk.
- **The loader is NOT a CSV parser.** No quoted fields, no embedded delimiters or newlines inside fields, no escape handling. It is a single-character-delimited line parser; "comma" is only a delimiter choice, not a format claim. This is **correct** for TPC-H `.tbl` (which contains none of these). CSV / RFC-4180 conformance is **not** claimed — also recorded in `docs/LIMITATIONS.md`.
- **The loader copies bytes, and mmap is not purely orthogonal.** Parsed values are copied into owned columns; there is no mmap / zero-copy ingest. Even with mmap, non-inline VARCHARs (>12B) would still have to be copied into the `StringHeap`, *or* their `StringRef`s would have to point into the mapping — in which case the mapping must outlive the table. So mmap ingest interacts with ADR 0004's German-string model and is not a drop-in orthogonal change.
- **DATE validation is shallow.** Proleptic-Gregorian; `days_from_civil` does no validation at all, and the loader checks only the `YYYY-MM-DD` shape, so impossible calendar dates (e.g. `2021-02-30`) are not rejected.
- **No schema inference.** The caller must supply the `Schema`.

## How to defend this at a whiteboard

- "Storage chunks *are* execution chunks (2048 rows), so the scan returns a `const DataChunk*` straight into the pipeline — zero copy, which is the whole reason ADR 0006's borrow model exists. The vector is built at load time and frozen before any scan, so those pointers are stable."
- "The borrow contract has two halves: the table outlives the query (so storage can't vanish), AND no downstream operator may retain the pointer past the synchronous `consume()` call — a hash-join build side or aggregate must materialize. Table-outlives-query alone is necessary, not sufficient."
- "DuckDB stores 122,880-row row groups (60 × its 2048 vector) subdivided into vectors; the row group is its unit of compression, parallel scan, and min/max zone maps. I use a flat list of 2048-row chunks — simpler for P2, but I pay ~60× more storage wrappers and give up compression and zone maps. Row groups are my next refinement, and the chunked layout makes that additive. (Parquet, by contrast, sizes row groups by bytes, not rows — there's no single industry number.)"
- "P8 isn't built. When it is, a morsel will be a *run of N chunks* claimed by an atomic fetch-add on a shared chunk cursor — lock-freedom comes from the atomic claim, not from chunk boundaries. Morsel size is decoupled from the 2048 vector size and will be tuned larger (Leis uses ~100K rows); a single chunk is too small to be a good morsel. Storage is just pre-partitioned at chunk granularity so no re-chunking is needed."
- "`from_chars`, not `strtod`/`stoi`: locale-independent by spec (`strtod` follows C `LC_NUMERIC`, `istream >>` the imbued locale — either can be flipped to comma-decimal and silently corrupt doubles), no allocation, and it flags trailing garbage so `12x` is an error, not a silent `12`. Float `from_chars` landed late in libstdc++ (GCC 11); my primary toolchain is clang-22/libc++, gcc-14 secondary — both verified."
- "Empty-field ⇒ NULL is a *policy* — text can't distinguish absent from empty — so the null token is configurable; set it to `\N` and empty means empty string."
- "DATE is `int32` days-since-1970 via Hinnant's `days_from_civil` (epoch = 0): O(1) integer math, no `<chrono>`/`mktime` locale/timezone cost, and date predicates become integer compares. It's proleptic Gregorian and the formula does *zero* validation — the loader only checks the `YYYY-MM-DD` shape, so `2021-02-30` slips through. Fine for TPC-H, out of scope to harden in P2."
- "It is *not* a CSV parser — no quotes, escapes, or embedded delimiters; comma is just a delimiter option. Correct for `.tbl`, and I say so in LIMITATIONS.md rather than overclaiming CSV support."
- "I don't infer the schema — the caller supplies it. For an engine validated against DuckDB, explicit and reproducible beats heuristic."
