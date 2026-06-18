# 0002. C++23 with Homebrew LLVM/libc++ Toolchain and Dependency Strategy

## Status

Accepted

## Context

Strata is a vectorized, push-based analytical query engine targeting C++23. The
language choice is not incidental: the design leans on `std::expected` for
allocation-free error propagation in the per-batch hot path, on `std::print` /
`std::format` for ergonomic diagnostics at the query boundary, and on the
`<bit>` header (`std::countl_zero`, `std::popcount`, `std::bit_ceil`,
`std::has_single_bit`) for the bit-manipulation that shows up in hashing, bitmap
selection vectors, and power-of-two sizing of the `VECTOR_SIZE = 2048` batches.
These are library features, not just language features, and library support — and
its *deployment-target gating* — is exactly where toolchains differ in mid-2026.

The development machine is an Apple M3 Pro (arm64, ARM NEON, 128-bit SIMD). The
default system compiler is Apple clang 17, which ships with Xcode and is what
`/usr/bin/clang++` resolves to. An important factual correction to an earlier
draft of this ADR: **Apple clang 17's libc++ (macOS 26 SDK) does ship
`std::expected`, `std::print`, and `<bit>`, and they compile and run at the
machine's default deployment target.** The `<version>` feature macros match
Homebrew LLVM 22.1.7 (`__cpp_lib_expected = 202211`,
`__cpp_lib_print = 202207`, `__cpp_lib_bitops = 201907`). `std::expected` is not
availability-gated in Apple's libc++ at all, and `<bit>` is fully present. The
*only* relevant availability gate is on `std::print` / `std::format` of
floating-point (which routes through `std::to_chars`, introduced in the LLVM 18
libc++); that gate fires only when you target an **old** deployment target
(e.g. `-mmacosx-version-min=13.0`), not at the machine's current default. So
Apple clang 17 would, in fact, compile this design today.

The real concern is therefore not feature *absence* but **toolchain and
deployment-target variability**: contributors on different macOS versions and
deployment targets, an Ubuntu CI runner on a different stdlib, and the
back-deployment gate on `std::print`-of-floats all introduce variance we do not
want to reason about per call site. We need one pinned toolchain version that
behaves identically for every contributor and matches the spirit of the Ubuntu
runner, eliminating availability/deployment-target variance entirely — not a
toolchain chosen to dodge missing features.

We also need a dependency story. Strata pulls in Google Highway (portable SIMD
wrappers so the same kernels emit NEON on arm64 and AVX2 on x86_64), GoogleTest,
and Google Benchmark. These must resolve identically on a contributor's Mac and
on the Ubuntu x86_64 CI runner, and the resolution must be reproducible — a
silent minor-version bump in a dependency is exactly the kind of thing that turns
"DuckDB-validated" benchmark numbers into noise.

## Decision

1. **Compiler/stdlib:** Standardize on **Homebrew LLVM clang 22.1.7 + libc++**
   under `-std=c++23 -stdlib=libc++`. This is a version-pinning / CI-parity
   choice, *not* a forced one — Apple clang 17 would compile the design.
   Contributors `brew install llvm` and point `CMAKE_CXX_COMPILER` at
   `$(brew --prefix llvm)/bin/clang++`.

2. **Error type:** Use `std::expected<T, Error>` as the project-wide
   `Result<T, Error>` for fallible operations on the hot path. No exceptions in
   per-batch execution. Exceptions remain acceptable at the query-/setup-level
   boundary (plan construction, catalog lookup, file open).

3. **Dependencies:** Resolve each dependency via `find_package(<exact-name> CONFIG)`
   against Homebrew's installed config packages — `find_package(hwy CONFIG)`,
   `find_package(GTest CONFIG)`, `find_package(benchmark CONFIG)` (Highway 1.4.0,
   GoogleTest 1.17.0, Google Benchmark 1.9.5) — with a `FetchContent` fallback
   **pinned to explicit versions/tags** when the config package is not found
   (notably on the Ubuntu runner and on clean-room builds).

4. **Build hygiene:** Compile with `-Wall -Wextra -Wpedantic -Werror`, plus
   `-Wconversion` (and Clang's `-Wshorten-64-to-32`) where we specifically want
   index-narrowing caught. Maintain a preset split: `asan-ubsan`, `tsan`, and
   `release` (`-O3 -march=native -DNDEBUG`).

### Compiler and standard library

What we actually verified, and the scope of each claim, kept separate on purpose:

- **Verified (positive):** On the M3 Pro under Homebrew LLVM 22.1.7 + libc++ with
  `-std=c++23 -stdlib=libc++`, `std::expected`, `std::print`, and the `<bit>`
  helpers compile and run correctly. This proves the chosen toolchain *has* the
  features.
- **Verified (the comparator, honestly):** Apple clang 17 + libc++ on this same
  machine *also* compiles and runs `std::expected`, `std::print`, and `<bit>` at
  the default deployment target. The single observed gap is back-deployment:
  building `std::print`/`std::format` of floating-point with an older
  `-mmacosx-version-min` errors on the availability-gated `std::to_chars`. We do
  **not** claim Apple clang lacks these features, because it does not.

The defensible rationale for standardizing on Homebrew LLVM 22.1.7 is therefore
(a) a single, pinned toolchain version shared across all contributors, removing
deployment-target / availability variability entirely, (b) matching the spirit of
the Ubuntu CI runner so a contributor's local build and CI agree, and (c) staying
current on newer C++23/C++26 library work. It is a portability/reproducibility
decision, not a feature-necessity one.

The sysroot gotcha (verified): invoking the Homebrew `clang++` **directly** on
the command line failed because it searched for a nonexistent `MacOSX26.sdk` —
Homebrew LLVM does not locate the macOS SDK the way Apple's wrapper does. The
direct-invocation fix is to pass `-isysroot "$(xcrun --show-sdk-path)"`. In
practice we never invoke the compiler by hand: **CMake supplies
`CMAKE_OSX_SYSROOT` automatically** (it runs the `xcrun` query for us), so the
in-tree CMake/Ninja build is correct without any manual `-isysroot`. Contributors
therefore only need to (a) `brew install llvm` and (b) set `CMAKE_CXX_COMPILER`
to the Homebrew `clang++`; the SDK plumbing is handled. (`llvm` is keg-only,
which is exactly why `CMAKE_CXX_COMPILER` must point explicitly at
`$(brew --prefix llvm)/bin/clang++` rather than being on `PATH`.)

### `std::expected` as `Result<T, Error>`

The execution direction is push/data-flow: an operator's `consume()` receives a
batch from its child and pushes results to its parent. We adopt the **push
direction** of Neumann's produce/consume model ("Efficiently Compiling Efficient
Query Plans for Modern Hardware," VLDB/PVLDB 2011, HyPer) — but with an explicit
distinction the project's framing (and Kersten et al. 2018) demands: Neumann's
produce/consume is the framing for **data-centric code generation**, where
`consume()` *emits code* and the whole pipeline compiles to one tight loop with
no per-tuple virtual calls. Strata is **vectorized/interpreted** (MonetDB/X100;
DuckDB/Photon-style), so `consume()` is a **runtime method invoked once per
2048-row batch**, not a code-generation hook. We are pushing *batches*, not
compiling *pipelines*. The Neumann reference is for the push direction only, not
for compilation. That per-batch `consume()` call chain is the hot path. Two
properties matter there:

- **No hidden control flow, and an optimizer-visible failure edge.** Modern
  Itanium-ABI zero-cost exceptions are free on the *non-throwing happy path at
  runtime* — landing pads only execute when something is actually thrown, which on
  the hot path is never, and unwind tables (`.eh_frame` / `.gcc_except_table`) are
  metadata that do not execute. The legitimate, defensible costs of pervasive
  exceptions are: (1) binary-size / metadata bloat, and (2) the optimizer being
  unable to assume `noexcept` around potentially-throwing calls, which can
  genuinely inhibit inlining and instruction reordering across the per-batch call
  chain. `std::expected` makes the failure edge a plain branch the optimizer can
  see and schedule.
- **Allocation-free propagation.** `std::expected<T, Error>` is a stack value;
  propagating an error up the operator chain costs a move and a branch, not a heap
  allocation or an unwind. A bare-enum `Error` keeps `Result<T>` small enough for
  register-return; once we embed a fixed-capacity message/context buffer, the type
  grows — `sizeof(std::expected<long, {int code; char msg[32];}>)` is 40 bytes on
  this libc++, which under the AArch64 (and SysV x86-64) return convention is
  returned via a hidden pointer (sret) / memory, not in registers. It is still
  allocation-free and unwind-free; we just do not claim register residency for a
  type carrying an embedded message buffer.

Exceptions are not banned globally — that would be dogma. At the **query/setup
boundary** (parsing, binding, catalog access, opening Parquet/CSV inputs) the cost
of an exception is amortized against an entire query and the ergonomics win; we
catch at that boundary and convert to `Result` before entering execution.

**Fallback plan (not needed on this toolchain, stated for honesty and
portability):** had the chosen toolchain lacked `std::expected`, we would have
introduced a minimal hand-rolled `tl::expected`-shaped type — a tagged union of
`T` and `Error` with `has_value()`, `value()`, `error()`, and monadic
`and_then` / `transform` — behind the same `Result<T, Error>` alias, so call
sites would not change. Because both Homebrew LLVM 22.1.7 and Apple clang 17 ship
a conforming `std::expected`, we use the standard type directly and keep the alias
(`template <class T> using Result = std::expected<T, Error>;`) purely as a
naming/affordance layer and as the seam where that fallback could be reinstated.

### Dependency pinning

Each dependency is resolved with `find_package(<exact-name> CONFIG REQUIRED)`
first, against the Homebrew-installed config packages. The `find_package` package
names are *not* the human-readable display names, and getting them wrong silently
drops you onto the slow `FetchContent` path:

- Highway 1.4.0 → **`find_package(hwy CONFIG)`** → `hwy::hwy`
  (the config is `hwy-config.cmake`; the name is **`hwy`**, not `highway`/`Highway`)
- GoogleTest 1.17.0 → **`find_package(GTest CONFIG)`** → `GTest::gtest_main`
- Google Benchmark 1.9.5 → **`find_package(benchmark CONFIG)`** → `benchmark::benchmark`

When the config package is absent, CMake falls back to `FetchContent_Declare`
**pinned to explicit version tags/commits matching the versions above**, not to
`master`/`main`. The Ubuntu x86_64 runner needs the fallback simply because it is
**not a Homebrew environment** — it provisions via apt and does not have these
Homebrew config packages installed (this has nothing to do with keg-only status;
of these formulae only `llvm` is keg-only, and `hwy`/`GTest`/`benchmark` are
not). Clean checkouts without the libraries installed also take the fallback.
Pinning the fallback keeps the compiled code — the SIMD wrappers, the test
harness, the benchmark harness — identical everywhere.

This is a deliberate two-path resolution. The honest tradeoffs:

- **`find_package` path** is fast (no source build) but depends on the
  contributor's Homebrew state. Homebrew rolls dependencies forward; a
  `brew upgrade` can drift Highway/GTest/GBench off the pinned versions. We
  mitigate by documenting the exact package names and expected versions, and by
  having CI exercise the pinned `FetchContent` path so the "known-good" versions
  are always built and tested somewhere.
- **`FetchContent` fallback path** is fully reproducible and self-contained but
  pays a real cost: it builds the dependencies from source, which lengthens cold
  CI builds and clean local builds. We accept that cost on the runner in exchange
  for byte-for-byte reproducible dependency versions.

### Warnings and preset split

`-Wall -Wextra -Wpedantic -Werror` is on for all first-party code. A vectorized
engine accumulates exactly the classes of latent bug these catch — sign-compare
in selection-vector loops (`-Wsign-compare`, in `-Wextra`) and unused results
from `[[nodiscard]]` `Result` returns (`-Wunused-result` / the `[[nodiscard]]`
diagnostic). One precision note: `-Wall`/`-Wextra` do **not** diagnose general
implicit narrowing — narrowing is only standard-diagnosed in braced-init (`{}`)
contexts. For narrowing in batch-index arithmetic (e.g. 64→32-bit) we therefore
additionally enable `-Wconversion` (and Clang's `-Wshorten-64-to-32`) on
first-party code. Treating these as errors keeps the hot path honest.
(Third-party `FetchContent` sources are built without `-Werror`; we do not own
their warning hygiene.)

Presets:

- **`asan-ubsan`** — correctness gate for tests; catches OOB on selection vectors,
  UB in bit tricks, lifetime bugs in batch buffers.
- **`tsan`** — provisioned for the morsel-driven parallel paths (Leis, Boncz,
  Kemper, Neumann, "Morsel-Driven Parallelism: A NUMA-Aware Query Evaluation
  Framework for the Many-Core Age," SIGMOD 2014, HyPer), where the scheduler and
  shared hash tables are the obvious data-race surface, once the parallel
  scheduler and shared hash tables land. (Stated as provisioned, not done, to
  avoid implying parallelism that is not yet built.)
- **`release`** — `-O3 -march=native -DNDEBUG` for benchmarking against DuckDB.
  `-march=native` is correct for benchmarking on the machine under test (NEON on
  the M3 Pro, AVX2 on the x86 runner) and is the only configuration whose numbers
  we report.

## Alternatives Considered

**Apple clang 17 (system default).** *Rejected — but for the honest reason.* Apple
clang 17 *does* provide `std::expected`, `std::print`, and `<bit>` at the
default deployment target, so this is **not** rejected for missing features. It
is rejected because (a) it varies across contributors' macOS versions and
deployment targets, (b) it carries the one real availability gate
(`std::print`/`std::format`-of-floats back-deploying to older
`-mmacosx-version-min`), and (c) it does not match a single pinned version shared
with CI. Standardizing on one Homebrew LLVM version removes that variance. The
only thing Apple clang buys is "no extra install," which is not worth the
variance.

**GCC as the primary macOS compiler.** *Rejected:* on arm64 macOS the first-class
path is clang/libc++. Beyond SDK/linker friction, mixing GCC + libstdc++ with the
libc++ ABI used by Apple/Homebrew libraries risks `std::` ABI mismatches — GCC's
libstdc++ and Clang's libc++ have different standard-library ABIs, so mixing
GCC-built and Clang-built objects (or linking against libc++-built system
libraries) invites ODR/ABI hazards. We do want GCC and/or clang exercised on the
Ubuntu side of CI to keep portability honest — so GCC has a role as a CI
compiler, just not as the primary Mac compiler.

**Exceptions everywhere (no `Result` type).** *Rejected:* zero-cost EH is free on
the happy path at runtime, but pervasive exceptions bloat binaries with unwind
metadata and, more importantly, prevent the optimizer from assuming `noexcept`
around potentially-throwing calls, which can inhibit inlining and reordering
across the per-batch call chain. We want the failure edge to be a plain branch the
optimizer can see, taken explicitly and allocation-free where it is taken millions
of times per query.

**Error codes / `errno`-style `int` returns.** *Rejected:* loses type information
on the success value, invites unchecked returns, and lacks the monadic
composition (`and_then` / `transform`) that keeps the operator chain readable.
`[[nodiscard]] std::expected` gives us checked, typed, composable errors.

**Vendoring dependencies as git submodules.** *Rejected in favor of
`find_package` + pinned `FetchContent`:* submodules give reproducibility but lose
the fast path of using a contributor's already-installed Homebrew packages, and
they add the well-known submodule UX friction. The chosen scheme keeps the fast
local path while retaining a reproducible, pinned fallback.

**`FetchContent` for everything (no `find_package`).** *Rejected:* maximally
reproducible but pays the from-source build cost on every contributor's machine,
not just CI. Using Homebrew config packages locally keeps day-to-day builds fast;
the pinned fallback covers reproducibility where it actually matters.

## Consequences

**Wins**

- One pinned toolchain (Homebrew LLVM 22.1.7 + libc++) shared across contributors
  and aligned with CI, removing deployment-target / availability variability —
  including the `std::print`-of-floats back-deployment gate — rather than leaving
  it per-machine.
- C++23 library features the design uses (`std::expected`, `std::print`, `<bit>`)
  are available and verified, with no conditional compilation. (Verified on both
  Homebrew LLVM and Apple clang; we standardize on Homebrew for pinning, not
  necessity.)
- Hot-path errors propagate explicitly and allocation-free; exceptions are
  confined to the query/setup boundary where their cost is amortized.
- Dependencies resolve fast locally (Homebrew config packages, correct names) and
  reproducibly in CI (pinned `FetchContent`), with identical Highway/GTest/GBench
  versions across arm64 and x86_64.
- Warnings-as-errors (plus targeted `-Wconversion`) and the sanitizer/release
  split keep the vectorized hot path honest while still giving clean
  `-O3 -march=native` numbers for the DuckDB comparison.

**Tradeoffs / costs accepted**

- **Heavier contributor setup.** Contributors must `brew install llvm` (keg-only)
  and point `CMAKE_CXX_COMPILER` at `$(brew --prefix llvm)/bin/clang++`; the
  system compiler would work but we standardize off it for pinning. We accept this
  friction for a current, version-pinned, CI-matching toolchain.
- **A CI matrix that must exercise both toolchains.** To keep portability claims
  honest, CI runs **both** Homebrew-LLVM-on-macOS (arm64, NEON benchmarks,
  ASan/UBSan) **and** gcc/clang-on-Ubuntu (x86_64, AVX2 benchmarks, sanitizers).
  Two jobs, two SIMD targets, two stdlib configurations — more CI surface to
  maintain, but it is precisely what lets us claim the engine is portable and lets
  us publish honest AVX2 numbers from real x86 hardware.
- **Homebrew version drift on the local fast path.** A `brew upgrade` can move
  Highway/GTest/GBench off the pinned versions; the pinned `FetchContent` path in
  CI is the source of truth that catches resulting discrepancies.
- **`FetchContent` build time.** The reproducible fallback builds dependencies
  from source, lengthening cold/CI builds. Accepted as the price of pinned
  reproducibility.
- **CI numbers are a regression signal, not headline performance.** Pinning makes
  the *compiled code* (SIMD wrappers, harnesses) identical across runs, removing
  dependency drift as a variable — but it does not make the *hardware/timing*
  comparable. GitHub-hosted x86_64 runners are shared, virtualized, and
  frequency-variable, so we treat their AVX2 microbenchmark numbers as a
  directional / regression signal only. Headline DuckDB-comparison numbers come
  from a controlled bare-metal x86 box (and the M3 Pro for NEON).
- **`-march=native` is machine-specific.** `-march=native` bakes in the build
  host's ISA extensions; the binary may fault with illegal-instruction on any CPU
  that lacks them, so it is not safely redistributable to older/different
  microarchitectures. It will run on feature-superset CPUs (e.g. an AVX2 build on
  a newer AVX-512 chip). This is intentional — we benchmark on the machine under
  test.

## How to defend this at a whiteboard

- "Apple clang 17 actually *ships* `std::expected`, `std::print`, and `<bit>` and
  compiles this design at the default deployment target — I tested it. I do not
  claim it's missing features. I standardize on Homebrew LLVM 22.1.7 for a
  different, honest reason: one pinned toolchain version across contributors,
  matching CI, removing deployment-target/availability variance — notably the
  `std::print`-of-floats back-deployment gate to older `-mmacosx-version-min`."
- "`std::expected` is the hot-path error type: explicit, allocation-free,
  unwind-free — a move plus a branch. A bare-enum `Error` fits in registers; once
  it carries a fixed message buffer the `expected` is ~40 bytes and returns via
  sret/memory, still allocation-free, just not register-resident. Exceptions live
  at the query/setup boundary and get converted to `Result` before execution."
- "Why not exceptions on the hot path? Zero-cost EH is free at runtime on the
  happy path — landing pads never run when nothing throws. The real costs are
  binary bloat and the optimizer not being able to assume `noexcept`, which
  inhibits inlining/reordering across the per-batch call chain. `expected` is a
  branch the optimizer can see."
- "Push direction is from Neumann's produce/consume (VLDB 2011), but Neumann is
  about data-centric *code generation*. Strata is a *vectorized interpreter*
  (MonetDB/X100; DuckDB/Photon), so `consume()` is a runtime method called once
  per 2048-row batch — pushing batches, not compiling pipelines. That
  compiled-vs-vectorized line is exactly what Kersten et al. 2018 is about."
- "Homebrew `clang++` invoked directly couldn't find the SDK (`MacOSX26.sdk`); the
  fix is `-isysroot $(xcrun --show-sdk-path)`, but CMake sets `CMAKE_OSX_SYSROOT`
  for us, so the in-tree build just works. `llvm` is keg-only, which is why
  `CMAKE_CXX_COMPILER` points explicitly at `$(brew --prefix llvm)/bin/clang++`."
- "Deps: the `find_package` names are non-obvious — `find_package(hwy CONFIG)` for
  Highway (not `highway`), `GTest`, and `benchmark`. `find_package(CONFIG)` for
  the fast local path, pinned `FetchContent` fallback for reproducibility and the
  Ubuntu runner — which needs it because it's an apt environment, not Homebrew, not
  because of keg-only status. Tradeoff is brew drift vs. FetchContent build time;
  CI builds the pinned path so the known-good versions are always tested."
- "`-Werror` on `-Wall -Wextra -Wpedantic`, plus `-Wconversion`/`-Wshorten-64-to-32`
  where I want index-narrowing caught — `-Wall`/`-Wextra` alone only flag
  narrowing in braced init. `-march=native` only in release because we benchmark
  on the machine under test — NEON on the M3, AVX2 on the x86 box."
- "CI runs both toolchains/ISAs on purpose. But I treat hosted-runner AVX2 numbers
  as a regression signal, not headline performance — GitHub runners are shared and
  frequency-variable. Pinning makes the code identical, not the hardware; headline
  numbers come from bare metal."
