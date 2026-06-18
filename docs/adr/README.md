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

Format: `NNNN-kebab-title.md`. Add a row above when you add a record.
