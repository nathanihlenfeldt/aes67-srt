#include "parallel_for.hpp"

#include <atomic>
#include <string>
#include <vector>

#include "test_framework.hpp"

TEST_CASE(parallel_for_runs_every_job_exactly_once) {
  aes67_srt::ParallelFor pool;
  std::string error;
  CHECK(pool.start(3, &error));
  CHECK(pool.started());

  // Many rounds, because a race that resets the counters under a worker only shows
  // up across generations, and one round would not.
  for (int round = 0; round < 200; ++round) {
    std::vector<std::atomic<int>> counters(64);
    for (std::atomic<int>& counter : counters) {
      counter.store(0);
    }
    pool.run(counters.size(), [&](size_t index) { counters[index].fetch_add(1); });
    for (size_t index = 0; index < counters.size(); ++index) {
      CHECK_EQ(counters[index].load(), 1);
    }
  }
}

TEST_CASE(parallel_for_runs_inline_with_no_workers_or_one_job) {
  aes67_srt::ParallelFor pool;
  int sum = 0;
  pool.run(10, [&](size_t index) { sum += static_cast<int>(index); });
  CHECK_EQ(sum, 45);

  pool.run(0, [&](size_t) { sum = -1; });
  CHECK_EQ(sum, 45);
}