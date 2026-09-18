#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace aes67_srt {

/**
 * Runs independent jobs across a fixed set of worker threads.
 *
 * Phase 2 needs one Opus encode per block, and eight encodes on one thread is
 * 43% of a Pi core (docs/research/opus.md). This spreads them across the cores
 * with threads that live for the engine's lifetime rather than being spawned per
 * period, because at fifty periods a second a thread per period is churn for
 * nothing.
 *
 * `run` blocks until every job is done, and the **caller works too** — it is not
 * a dispatcher standing idle. Jobs must be independent: they are handed out in
 * whatever order the workers reach them.
 */
class ParallelFor {
 public:
  ParallelFor() = default;
  ~ParallelFor();
  ParallelFor(const ParallelFor&) = delete;
  ParallelFor& operator=(const ParallelFor&) = delete;

  /** Start |workers| threads. Zero or one means "run jobs inline". */
  bool start(size_t workers, std::string* error);

  /** Stop and join the workers. Safe to call more than once. */
  void stop();

  bool started() const;

  /**
   * Run job(0..count-1) and return when all have run. |count| of 0 does nothing
   * and 1 runs inline, so a single-block link never touches the threads.
   */
  void run(size_t count, const std::function<void(size_t)>& job);

 private:
  void worker();

  std::vector<std::thread> workers_;
  std::mutex mutex_;
  std::condition_variable work_cv_;
  std::condition_variable done_cv_;
  const std::function<void(size_t)>* job_ = nullptr;
  size_t count_ = 0;
  size_t remaining_ = 0;
  size_t active_ = 0;
  size_t generation_ = 0;
  std::atomic<size_t> next_{0};
  bool stopping_ = false;
};

}  // namespace aes67_srt