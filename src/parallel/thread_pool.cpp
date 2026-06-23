#include "strata/parallel/thread_pool.hpp"

#include <utility>

namespace strata {

ThreadPool::ThreadPool(std::size_t num_threads) {
  if (num_threads == 0) num_threads = 1;
  workers_.reserve(num_threads);
  for (std::size_t i = 0; i < num_threads; ++i) workers_.push_back(std::make_unique<Worker>());
  threads_.reserve(num_threads);
  for (std::size_t i = 0; i < num_threads; ++i) {
    threads_.emplace_back([this, i] { WorkerLoop(i); });
  }
}

ThreadPool::~ThreadPool() {
  stop_.store(true);
  {
    std::lock_guard<std::mutex> lk(work_mu_);  // serialize with workers' wait predicate
  }
  work_cv_.notify_all();
  for (std::thread& t : threads_) {
    if (t.joinable()) t.join();
  }
}

void ThreadPool::Submit(std::size_t target, Task t) {
  {
    std::lock_guard<std::mutex> lk(workers_[target]->mu);
    workers_[target]->queue.push_back(std::move(t));
  }
  outstanding_.fetch_add(1, std::memory_order_release);
  available_.fetch_add(1, std::memory_order_release);
  {
    std::lock_guard<std::mutex> lk(work_mu_);  // close the lost-wakeup window
  }
  work_cv_.notify_one();
}

bool ThreadPool::TryGetTask(std::size_t id, Task& out) {
  // Own deque first (LIFO — better locality for recently-pushed work).
  {
    std::lock_guard<std::mutex> lk(workers_[id]->mu);
    if (!workers_[id]->queue.empty()) {
      out = std::move(workers_[id]->queue.back());
      workers_[id]->queue.pop_back();
      available_.fetch_sub(1, std::memory_order_acq_rel);
      return true;
    }
  }
  // Steal FIFO from other workers (oldest task — likely the largest remaining run).
  const std::size_t n = workers_.size();
  for (std::size_t k = 1; k < n; ++k) {
    const std::size_t j = (id + k) % n;
    std::lock_guard<std::mutex> lk(workers_[j]->mu);
    if (!workers_[j]->queue.empty()) {
      out = std::move(workers_[j]->queue.front());
      workers_[j]->queue.pop_front();
      available_.fetch_sub(1, std::memory_order_acq_rel);
      return true;
    }
  }
  return false;
}

void ThreadPool::WorkerLoop(std::size_t id) {
  for (;;) {
    Task t;
    if (TryGetTask(id, t)) {
      t(id);  // run the task; passes THIS worker's id for thread-local indexing
      outstanding_.fetch_sub(1, std::memory_order_acq_rel);
      {
        std::lock_guard<std::mutex> lk(done_mu_);  // close the lost-wakeup window
      }
      done_cv_.notify_all();
      continue;
    }
    std::unique_lock<std::mutex> lk(work_mu_);
    work_cv_.wait(lk, [this] {
      return stop_.load(std::memory_order_acquire) ||
             available_.load(std::memory_order_acquire) > 0;
    });
    if (stop_.load(std::memory_order_acquire) && available_.load(std::memory_order_acquire) == 0) {
      return;
    }
  }
}

void ThreadPool::ParallelFor(std::size_t num_tasks,
                             const std::function<void(std::size_t, std::size_t)>& body) {
  if (num_tasks == 0) return;
  const std::size_t n = workers_.size();
  for (std::size_t i = 0; i < num_tasks; ++i) {
    Submit(i % n, [&body, i](std::size_t worker_id) { body(i, worker_id); });
  }
  // Block until every submitted task has completed. (ParallelFor is synchronous,
  // so outstanding_ returns to 0 exactly when this batch finishes.)
  std::unique_lock<std::mutex> lk(done_mu_);
  done_cv_.wait(lk, [this] { return outstanding_.load(std::memory_order_acquire) == 0; });
}

}  // namespace strata
