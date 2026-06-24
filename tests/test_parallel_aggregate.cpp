#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "strata/data/data_chunk.hpp"
#include "strata/exec/hash_aggregate.hpp"
#include "strata/exec/result_collector.hpp"
#include "strata/parallel/parallel_aggregate.hpp"
#include "strata/parallel/thread_pool.hpp"
#include "strata/storage/columnar_table.hpp"

namespace strata {
namespace {

Schema ResultSchema() {  // [k, sum(v), count(*), min(v), max(v)]
  return Schema(std::vector<ColumnDef>{{"k", TypeId::kInt32},
                                       {"s", TypeId::kInt64},
                                       {"c", TypeId::kInt64},
                                       {"mn", TypeId::kInt32},
                                       {"mx", TypeId::kInt32}});
}

// Table (k, v): k = i % num_keys, v = i.
ColumnarTable MakeTable(std::size_t n, std::int32_t num_keys) {
  Schema s(std::vector<ColumnDef>{{"k", TypeId::kInt32}, {"v", TypeId::kInt32}});
  ColumnarTable t(s);
  std::size_t done = 0;
  while (done < n) {
    const std::size_t b = std::min(kVectorSize, n - done);
    DataChunk c;
    c.Initialize(s.types(), kVectorSize);
    for (std::size_t i = 0; i < b; ++i) {
      const std::int32_t idx = static_cast<std::int32_t>(done + i);
      c.column(0).Set<std::int32_t>(i, idx % num_keys);
      c.column(1).Set<std::int32_t>(i, idx);
    }
    c.SetSize(b);
    t.AppendChunk(std::move(c));
    done += b;
  }
  return t;
}

std::vector<GroupKey> Keys() { return {{0, TypeId::kInt32}}; }
std::vector<AggregateSpec> Specs() {
  return {{AggFunc::kSum, 1, TypeId::kInt32},
          {AggFunc::kCountStar},
          {AggFunc::kMin, 1, TypeId::kInt32},
          {AggFunc::kMax, 1, TypeId::kInt32}};
}

using Row = std::array<std::int64_t, 4>;  // {sum, count, min, max}
std::map<std::int32_t, Row> ToMap(const ResultCollector& c) {
  std::map<std::int32_t, Row> m;
  for (std::size_t r = 0; r < c.row_count(); ++r) {
    m[c.Get<std::int32_t>(r, 0)] = {c.Get<std::int64_t>(r, 1), c.Get<std::int64_t>(r, 2),
                                    c.Get<std::int32_t>(r, 3), c.Get<std::int32_t>(r, 4)};
  }
  return m;
}

ResultCollector SerialAggregate(const ColumnarTable& t) {
  ResultCollector out(ResultSchema());
  HashAggregate agg(Keys(), Specs(), out);
  for (std::size_t i = 0; i < t.chunk_count(); ++i) agg.Consume(t.chunk(i));
  agg.Finalize();
  return out;
}

TEST(ParallelAggregate, MatchesSerialBitIdentical) {
  const ColumnarTable t = MakeTable(50'000, 100);
  const ResultCollector serial = SerialAggregate(t);

  for (const std::size_t nthreads : {std::size_t{1}, std::size_t{2}, std::size_t{4}, std::size_t{8}}) {
    ThreadPool pool(nthreads);
    ResultCollector par(ResultSchema());
    ParallelAggregate(pool, t, Keys(), Specs(), par);
    EXPECT_EQ(ToMap(par), ToMap(serial)) << "nthreads=" << nthreads;  // integer aggs: bit-identical
    EXPECT_EQ(par.row_count(), 100u);
  }
}

TEST(ParallelAggregate, GlobalMatchesSerial) {
  const ColumnarTable t = MakeTable(30'000, 1);  // single key -> effectively global-ish
  Schema gs(std::vector<ColumnDef>{{"s", TypeId::kInt64}, {"c", TypeId::kInt64}});

  ResultCollector serial(gs);
  HashAggregate sa({}, {{AggFunc::kSum, 1, TypeId::kInt32}, {AggFunc::kCountStar}}, serial);
  for (std::size_t i = 0; i < t.chunk_count(); ++i) sa.Consume(t.chunk(i));
  sa.Finalize();

  ThreadPool pool(8);
  ResultCollector par(gs);
  ParallelAggregate(pool, t, {}, {{AggFunc::kSum, 1, TypeId::kInt32}, {AggFunc::kCountStar}}, par);

  ASSERT_EQ(serial.row_count(), 1u);
  ASSERT_EQ(par.row_count(), 1u);
  EXPECT_EQ(par.Get<std::int64_t>(0, 0), serial.Get<std::int64_t>(0, 0));  // sum
  EXPECT_EQ(par.Get<std::int64_t>(0, 1), serial.Get<std::int64_t>(0, 1));  // count
  EXPECT_EQ(par.Get<std::int64_t>(0, 1), 30'000);
}

// VARCHAR group keys exercise the merge's string-copy / string-compare path
// (CopyKeyFromRow / KeysEqualRows varchar branches) that integer keys don't.
ColumnarTable MakeVarcharTable(std::size_t n, int num_keys) {
  Schema s(std::vector<ColumnDef>{{"k", TypeId::kVarchar}, {"v", TypeId::kInt32}});
  ColumnarTable t(s);
  std::size_t done = 0;
  while (done < n) {
    const std::size_t b = std::min(kVectorSize, n - done);
    DataChunk c;
    c.Initialize(s.types(), kVectorSize);
    for (std::size_t i = 0; i < b; ++i) {
      const std::int32_t idx = static_cast<std::int32_t>(done + i);
      const std::string key = "k" + std::to_string(idx % num_keys);
      c.column(0).Set<StringRef>(i, c.column(0).AddString(key));
      c.column(1).Set<std::int32_t>(i, idx);
    }
    c.SetSize(b);
    t.AppendChunk(std::move(c));
    done += b;
  }
  return t;
}

TEST(ParallelAggregate, VarcharGroupKeysMatchSerial) {
  const ColumnarTable t = MakeVarcharTable(40'000, 50);
  const std::vector<GroupKey> keys{{0, TypeId::kVarchar}};
  const std::vector<AggregateSpec> specs{{AggFunc::kSum, 1, TypeId::kInt32}, {AggFunc::kCountStar}};
  const Schema rs(std::vector<ColumnDef>{
      {"k", TypeId::kVarchar}, {"s", TypeId::kInt64}, {"c", TypeId::kInt64}});

  auto to_map = [](const ResultCollector& c) {
    std::map<std::string, std::pair<std::int64_t, std::int64_t>> m;
    for (std::size_t r = 0; r < c.row_count(); ++r) {
      m[c.GetString(r, 0)] = {c.Get<std::int64_t>(r, 1), c.Get<std::int64_t>(r, 2)};
    }
    return m;
  };

  ResultCollector serial(rs);
  HashAggregate sa(keys, specs, serial);
  for (std::size_t i = 0; i < t.chunk_count(); ++i) sa.Consume(t.chunk(i));
  sa.Finalize();

  ThreadPool pool(8);
  ResultCollector par(rs);
  ParallelAggregate(pool, t, keys, specs, par);

  EXPECT_EQ(par.row_count(), 50u);
  EXPECT_EQ(to_map(par), to_map(serial));
}

TEST(ParallelAggregate, StressManyThreadsManyRows) {
  // Large table + 8 threads, run a few times — exercises stealing/merge under TSan.
  const ColumnarTable t = MakeTable(200'000, 500);
  const ResultCollector serial = SerialAggregate(t);
  ThreadPool pool(8);
  for (int round = 0; round < 3; ++round) {
    ResultCollector par(ResultSchema());
    ParallelAggregate(pool, t, Keys(), Specs(), par);
    EXPECT_EQ(par.row_count(), 500u);
    EXPECT_EQ(ToMap(par), ToMap(serial));
  }
}

}  // namespace
}  // namespace strata
