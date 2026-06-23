# Architecture Decision Records

Each ADR captures one significant decision: the context, the decision, the
alternatives we rejected and *why*, and the tradeoff we accepted. They are the
project's design rationale and double as interview study notes — written to be
defended at a whiteboard.

| ADR | Title | Status |
|-----|-------|--------|
| [0001](0001-push-based-execution.md) | Push-based pipeline execution (vs pull-based Volcano) | Accepted |
| [0002](0002-cxx23-homebrew-llvm-toolchain.md) | C++23 with Homebrew LLVM/libc++ toolchain and dependency strategy | Accepted |
| [0003](0003-validity-mask.md) | Validity (NULL) representation: a 1=valid bitmask with an all-valid fast path | Accepted |
| [0004](0004-string-layout.md) | String layout: the 16-byte German/Umbra string and an arena StringHeap | Accepted |
| [0005](0005-vector-ownership-and-kinds.md) | Vector memory ownership (owning, 64-byte-aligned) and kinds (flat + constant) | Accepted |
| [0006](0006-pipeline-sink-interfaces.md) | Push-based execution scaffolding: Source / Sink / Pipeline | Accepted |
| [0007](0007-columnar-storage-and-loader.md) | In-memory columnar storage (ColumnarTable) and the delimited loader | Accepted |
| [0008](0008-simd-kernels-and-the-ceiling.md) | SIMD kernels: per-batch dispatch boundary, Highway runtime dispatch, and the SIMD ceiling | Accepted |
| [0009](0009-vectorized-expression-eval-and-3vl.md) | Vectorized expression evaluation and three-valued NULL logic | Accepted |
| [0010](0010-aggregate-hash-table.md) | Aggregate hash table: open addressing, linear probing, salt, and a row layout | Accepted |
| [0011](0011-aggregate-state-and-overflow.md) | Aggregate state, NULL semantics, and the SUM overflow policy | Accepted |
| [0012](0012-hash-join.md) | Hash join: build/probe, chained build table, row layout, vectorized match gather | Accepted |
| [0013](0013-sort-limit-topn.md) | Sort, Limit, Top-N: comparator-based columnar sort, a bounded heap, deferred normalized keys | Accepted |
| [0014](0014-sql-frontend-and-optimizer.md) | SQL front-end: hand-written parser, logical plan IR, and a rule-based optimizer | Accepted |
| [0015](0015-morsel-driven-parallelism.md) | Morsel-driven parallelism: a work-stealing thread pool, thread-local aggregation, and a merge | Accepted — primitives + parallel-aggregation path built and tested; not yet wired into the SQL executor |

Format: `NNNN-kebab-title.md`. Add a row above when you add a record.
