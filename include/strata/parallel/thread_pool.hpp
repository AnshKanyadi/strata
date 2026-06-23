#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace strata {

// A work-stealing thread pool (ADR 0015). Each worker owns a MUTEX-GUARDED task
// deque; a worker runs its own deque LIFO and STEALS FIFO from other workers
// when idle. Mutex/atomic/condition-variable synchronization makes it TSan-clean
// by construction. ParallelFor dispatches one task per index across the workers,
// passing each task the id of the worker that runs it (for thread-local state),
// and blocks until all tasks complete.
class ThreadPool {
 public:
  explicit ThreadPool(std::size_t num_threads);
  ~ThreadPool();
  ThreadPool(const ThreadPool&) = delete;
  ThreadPool& operator=(const ThreadPool&) = delete;

  std::size_t size() const noexcept { return threads_.size(); }

  // Runs body(task_index, worker_id) for task_index in [0, num_tasks). Blocks
  // until every task has completed. Not re-entrant / not called concurrently.
  void ParallelFor(std::size_t num_tasks,
                   const std::function<void(std::size_t, std::size_t)>& body);

 private:
  using Task = std::function<void(std::size_t)>;  // arg = worker id that runs it
  struct Worker {
    std::mutex mu;
    std::deque<Task> queue;
  };

  void WorkerLoop(std::size_t id);
  bool TryGetTask(std::size_t id, Task& out);
  void Submit(std::size_t target, Task t);

  std::vector<std::unique_ptr<Worker>> workers_;  // unique_ptr: Worker holds a mutex
  std::vector<std::thread> threads_;

  std::atomic<bool> stop_{false};
  std::atomic<std::size_t> available_{0};    // tasks queued but not yet popped
  std::atomic<std::size_t> outstanding_{0};  // tasks submitted but not yet completed

  std::mutex work_mu_;             // guards the parking condition (with available_/stop_)
  std::condition_variable work_cv_;
  std::mutex done_mu_;             // guards the completion condition (with outstanding_)
  std::condition_variable done_cv_;
};

}  // namespace strata
