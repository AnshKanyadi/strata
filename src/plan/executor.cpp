#include "strata/plan/executor.hpp"

#include <cstddef>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "strata/common/error.hpp"
#include "strata/data/column_ops.hpp"
#include "strata/data/data_chunk.hpp"
#include "strata/data/selection_vector.hpp"
#include "strata/data/vector.hpp"
#include "strata/exec/filter.hpp"
#include "strata/exec/hash_aggregate.hpp"
#include "strata/exec/limit.hpp"
#include "strata/exec/project.hpp"
#include "strata/exec/scan.hpp"
#include "strata/exec/sort.hpp"
#include "strata/exec/top_n.hpp"

namespace strata {
namespace {

struct ExecFail {
  std::string msg;
};
[[noreturn]] void Fail(std::string m) { throw ExecFail{std::move(m)}; }

Schema OutputSchema(const LogicalNode& n) {
  std::vector<ColumnDef> cols;
  cols.reserve(n.out_types.size());
  for (std::size_t i = 0; i < n.out_types.size(); ++i) {
    cols.push_back({n.out_names[i], n.out_types[i]});
  }
  return Schema(std::move(cols));
}

std::vector<SortKey> ExtractSortKeys(const std::vector<OrderItem>& order) {
  std::vector<SortKey> keys;
  for (const OrderItem& o : order) {
    if (o.expr->kind != ExprKind::kColumnRef) Fail("ORDER BY must be a column reference (P7)");
    keys.push_back({o.expr->column_index, o.expr->type, o.ascending, o.nulls_first});
  }
  return keys;
}

}  // namespace

Result<std::unique_ptr<ResultCollector>> ExecutePlan(LogicalNode& root, const Catalog& catalog) {
  try {
    // 1. Decompose the (single-table) plan into its components.
    LogicalNode* limit = nullptr;
    LogicalNode* order = nullptr;
    LogicalNode* sc = nullptr;  // schema-changer: Project or Aggregate
    LogicalNode* filter = nullptr;
    LogicalNode* cur = &root;
    if (cur->op == LogicalOp::kLimit) { limit = cur; cur = cur->children[0].get(); }
    if (cur->op == LogicalOp::kOrder) { order = cur; cur = cur->children[0].get(); }
    if (cur->op == LogicalOp::kProject || cur->op == LogicalOp::kAggregate) {
      sc = cur;
      cur = cur->children[0].get();
    }
    if (cur->op == LogicalOp::kFilter) { filter = cur; cur = cur->children[0].get(); }
    if (cur->op != LogicalOp::kGet) Fail("unsupported plan shape (P7 executes single-table plans)");
    LogicalNode* get = cur;
    if (sc == nullptr) Fail("plan has no projection or aggregate");

    const ColumnarTable* table = catalog.Find(get->table);
    if (table == nullptr) Fail("unknown table: " + get->table);

    // 2. Result collector (owns the materialized output).
    auto collector = std::make_unique<ResultCollector>(OutputSchema(root));

    // 3. Post-aggregate / post-projection sink chain (Sort/TopN/Limit -> collector).
    std::vector<SortKey> keys;
    if (order != nullptr) keys = ExtractSortKeys(order->order);
    std::unique_ptr<Sink> sortish;   // Sort or TopN
    std::unique_ptr<Limit> limit_op;
    Sink* first_post = collector.get();
    if (order != nullptr && limit != nullptr) {
      sortish = std::make_unique<TopN>(keys, limit->limit, *collector);  // ORDER BY ... LIMIT k
      first_post = sortish.get();
    } else if (order != nullptr) {
      sortish = std::make_unique<Sort>(keys, *collector);
      first_post = sortish.get();
    } else if (limit != nullptr) {
      limit_op = std::make_unique<Limit>(limit->limit, *collector);
      first_post = limit_op.get();
    }

    // 4. Projection-pushdown scan shape.
    const std::vector<std::size_t>& read = get->read_columns;
    bool identity = read.size() == table->schema().size();
    if (identity) {
      for (std::size_t i = 0; i < read.size(); ++i) {
        if (read[i] != i) { identity = false; break; }
      }
    }

    // 5. Driver projection (over the scan rows) + the first sink it feeds.
    std::unique_ptr<HashAggregate> agg_op;
    std::vector<ExprPtr> driver_exprs;
    Sink* first_sink = first_post;
    if (sc->op == LogicalOp::kAggregate) {
      const std::size_t g = sc->group_keys.size();
      for (ExprPtr& gk : sc->group_keys) driver_exprs.push_back(std::move(gk));
      std::vector<GroupKey> gkeys;
      for (std::size_t i = 0; i < g; ++i) gkeys.push_back({i, driver_exprs[i]->type});
      std::vector<AggregateSpec> specs;
      std::size_t argpos = 0;
      for (AggregateItem& a : sc->aggregates) {
        if (a.arg) {
          const TypeId at = a.arg->type;
          driver_exprs.push_back(std::move(a.arg));
          specs.push_back({a.func, g + argpos, at});
          ++argpos;
        } else {
          specs.push_back({a.func, 0, TypeId::kInt32});  // COUNT(*) reads no column
        }
      }
      agg_op = std::make_unique<HashAggregate>(std::move(gkeys), std::move(specs), *first_post);
      first_sink = agg_op.get();
    } else {  // Project
      for (ExprPtr& p : sc->projections) driver_exprs.push_back(std::move(p));
    }
    Project driver(std::move(driver_exprs));

    std::unique_ptr<Filter> filter_op;
    if (filter != nullptr) filter_op = std::make_unique<Filter>(std::move(filter->predicate));

    // 6. Drive the scan: (project to read_columns) -> filter -> driver projection -> first sink.
    std::vector<TypeId> proj_types;
    if (!identity) {
      for (std::size_t c : read) proj_types.push_back(table->schema().column(c).type);
    }
    Scan scan(*table);
    while (const DataChunk* chunk = scan.GetChunk()) {
      const DataChunk* in = chunk;
      DataChunk projected;
      if (!identity) {
        projected.Initialize(proj_types, kVectorSize);
        for (std::size_t i = 0; i < read.size(); ++i) {
          CopyColumn(chunk->column(read[i]), projected.column(i), chunk->size());
        }
        projected.SetSize(chunk->size());
        in = &projected;
      }
      SelectionVector sel;  // identity by default (no WHERE)
      std::size_t count = in->size();
      if (filter_op) count = filter_op->Select(*in, sel);
      DataChunk out;
      out.Initialize(driver.output_types(), kVectorSize);
      driver.Execute(*in, sel, count, out);
      first_sink->Consume(out);
    }
    first_sink->Finalize();

    return collector;
  } catch (const ExecFail& e) {
    return Err(ErrorCode::kInternal, e.msg);
  }
}

}  // namespace strata
