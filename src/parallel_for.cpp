#include "parallel_for.hpp"

#include <system_error>

namespace aes67_srt {

ParallelFor::~ParallelFor() {
  stop();
}

bool ParallelFor::start(size_t workers, std::string* error) {
  stop();
  if (workers <= 1) {
    return true;  // inline execution; no threads to start
  }
  try {
    for (size_t index = 0; index < workers; ++index) {
      workers_.emplace_back([this] { worker(); });
    }
  } catch (const std::system_error& failure) {
    if (error != nullptr) {
      *error = std::string("cannot start worker threads: ") + failure.what();
    }
    stop();
    return false;
  }
  return true;
}

void ParallelFor::stop() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (workers_.empty()) {
      return;
    }
    stopping_ = true;
  }
  work_cv_.notify_all();
  for (std::thread& worker_thread : workers_) {
    if (worker_thread.joinable()) {
      worker_thread.join();
    }
  }
  workers_.clear();
  stopping_ = false;
}

bool ParallelFor::started() const {
  return !workers_.empty();
}

void ParallelFor::worker() {
  size_t seen = 0;
  for (;;) {
    {
      std::unique_lock<std::mutex> lock(mutex_);
      work_cv_.wait(lock, [&] { return stopping_ || generation_ != seen; });
      if (stopping_) {
        return;
      }
      seen = generation_;
      ++active_;
    }
    // Pull jobs until the generation's count is exhausted. The index is atomic so
    // no lock is held while a job runs.
    for (;;) {
      const size_t index = next_.fetch_add(1);
      if (index >= count_) {
        break;
      }
      (*job_)(index);
      std::lock_guard<std::mutex> lock(mutex_);
      if (--remaining_ == 0) {
        done_cv_.notify_all();
      }
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (--active_ == 0) {
      done_cv_.notify_all();
    }
  }
}

void ParallelFor::run(size_t count, const std::function<void(size_t)>& job) {
  if (count == 0) {
    return;
  }
  if (workers_.empty() || count == 1) {
    for (size_t index = 0; index < count; ++index) {
      job(index);
    }
    return;
  }

  {
    std::lock_guard<std::mutex> lock(mutex_);
    job_ = &job;
    count_ = count;
    next_.store(0);
    remaining_ = count;
    ++generation_;
  }
  work_cv_.notify_all();

  // The caller works too, so N workers means N+1 threads on the problem.
  for (;;) {
    const size_t index = next_.fetch_add(1);
    if (index >= count_) {
      break;
    }
    job(index);
    std::lock_guard<std::mutex> lock(mutex_);
    if (--remaining_ == 0) {
      done_cv_.notify_all();
    }
  }

  // Wait for every job and for the workers to leave this generation, so the next
  // run() cannot reset the counters under a worker still pulling from them.
  std::unique_lock<std::mutex> lock(mutex_);
  done_cv_.wait(lock, [&] { return remaining_ == 0 && active_ == 0; });
  job_ = nullptr;
}

}  // namespace aes67_srt