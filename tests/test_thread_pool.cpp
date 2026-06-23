#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "strata/parallel/thread_pool.hpp"

namespace strata {
namespace {

TEST(ThreadPool, RunsEveryTaskExactlyOnce) {
  ThreadPool pool(4);
  const std::size_t n = 100'000;
  std::vector<std::atomic<int>> seen(n);  // count visits per task index
  for (auto& s : seen) s.store(0);
  pool.ParallelFor(n, [&](std::size_t i, std::size_t /*wid*/) { seen[i].fetch_add(1); });
  for (std::size_t i = 0; i < n; ++i) EXPECT_EQ(seen[i].load(), 1) << "task " << i;
}

TEST(ThreadPool, ThreadLocalAccumulationNoRace) {
  // Each worker accumulates into ITS OWN partial (no shared mutable state).
  ThreadPool pool(8);
  const std::size_t n = 200'000;
  std::vector<std::int64_t> partial(pool.size(), 0);
  pool.ParallelFor(n, [&](std::size_t i, std::size_t wid) {
    partial[wid] += static_cast<std::int64_t>(i);  // only worker `wid` touches partial[wid]
  });
  std::int64_t total = 0;
  for (const std::int64_t p : partial) total += p;
  const std::int64_t expected = static_cast<std::int64_t>(n) * static_cast<std::int64_t>(n - 1) / 2;
  EXPECT_EQ(total, expected);
}

TEST(ThreadPool, WorkerIdsInRange) {
  ThreadPool pool(4);
  std::atomic<int> bad{0};
  pool.ParallelFor(10'000, [&](std::size_t /*i*/, std::size_t wid) {
    if (wid >= 4) bad.fetch_add(1);
  });
  EXPECT_EQ(bad.load(), 0);
}

TEST(ThreadPool, RepeatedDispatchStress) {
  // Many ParallelFor calls back-to-back — stresses park/wake and steal paths.
  ThreadPool pool(6);
  std::int64_t grand = 0;
  for (int round = 0; round < 50; ++round) {
    const std::size_t n = 20'000;
    std::vector<std::int64_t> partial(pool.size(), 0);
    pool.ParallelFor(n, [&](std::size_t i, std::size_t wid) { partial[wid] += static_cast<std::int64_t>(i); });
    for (const std::int64_t p : partial) grand += p;
  }
  const std::int64_t per_round = static_cast<std::int64_t>(20'000) * 19'999 / 2;
  EXPECT_EQ(grand, per_round * 50);
}

TEST(ThreadPool, SingleThreadIsSerial) {
  ThreadPool pool(1);
  std::int64_t sum = 0;
  pool.ParallelFor(1000, [&](std::size_t i, std::size_t wid) {
    EXPECT_EQ(wid, 0u);
    sum += static_cast<std::int64_t>(i);  // single worker -> no race
  });
  EXPECT_EQ(sum, static_cast<std::int64_t>(1000) * 999 / 2);
}

}  // namespace
}  // namespace strata
