// The named shared-memory region (issue #37): how the plug-in's host process and
// the application reach the same audio block when they are unrelated processes.

#include "audio/shared_region.hpp"

#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "audio/shared_ring.hpp"
#include "test_framework.hpp"

namespace {

using aes67_srt::audio::shared_bytes_for;
using aes67_srt::audio::SharedAudio;
using aes67_srt::audio::SharedRegion;

std::string unique_region_name(const char* tag) {
#if defined(__unix__) || defined(__APPLE__)
  return std::string("/aes67-srt-test-") + std::to_string(getpid()) + "-" + tag;
#else
  return std::string("/aes67-srt-test-") + tag;
#endif
}

}  // namespace

TEST_CASE(shared_region_creates_then_a_second_open_attaches) {
  const std::string name = unique_region_name("attach");
  SharedRegion::unlink(name);
  const size_t bytes = shared_bytes_for(32, 2);

  SharedRegion first;
  bool created = false;
  std::string error;
  CHECK(first.open(name, bytes, &created, &error));
  CHECK(created);
  CHECK(first.is_open());

  SharedRegion second;
  bool created_again = true;
  CHECK(second.open(name, bytes, &created_again, &error));
  CHECK(!created_again);
  CHECK(second.data() != first.data() || first.data() != nullptr);

  SharedRegion::unlink(name);
}

TEST_CASE(shared_region_refuses_a_name_without_a_slash) {
  SharedRegion region;
  bool created = false;
  std::string error;
  CHECK(!region.open("no-leading-slash", 4096, &created, &error));
  CHECK(error.find("begin with") != std::string::npos);
  CHECK(!region.is_open());
}

TEST_CASE(shared_region_unlink_removes_the_name) {
  const std::string name = unique_region_name("unlink");
  SharedRegion::unlink(name);
  const size_t bytes = shared_bytes_for(16, 1);

  SharedRegion first;
  bool created = false;
  std::string error;
  CHECK(first.open(name, bytes, &created, &error));
  CHECK(created);
  first.close();

  CHECK(SharedRegion::unlink(name));

  // With the name gone, the next open creates a fresh region rather than attaching.
  SharedRegion second;
  bool created_again = false;
  CHECK(second.open(name, bytes, &created_again, &error));
  CHECK(created_again);
  SharedRegion::unlink(name);
}

#if defined(__unix__) || defined(__APPLE__)

TEST_CASE(shared_region_carries_audio_between_two_processes) {
  // The rendezvous end to end: the parent creates and initializes the block, the
  // child finds it **by name** in its own process and attaches, and a frame written
  // by the child comes out of the parent's ring. This is the shape the plug-in and
  // the application use.
  const std::string name = unique_region_name("xproc");
  SharedRegion::unlink(name);
  constexpr size_t capacity = 64;
  constexpr unsigned channels = 2;
  const size_t bytes = shared_bytes_for(capacity, channels);

  SharedRegion parent;
  bool created = false;
  std::string error;
  CHECK(parent.open(name, bytes, &created, &error));
  CHECK(created);
  SharedAudio audio;
  CHECK(audio.create(parent.data(), bytes, capacity, channels, &error));

  const pid_t pid = fork();
  CHECK(pid >= 0);
  if (pid == 0) {
    SharedRegion child;
    bool child_created = true;
    std::string child_error;
    if (!child.open(name, bytes, &child_created, &child_error) || child_created) {
      _exit(3);
    }
    SharedAudio child_audio;
    if (!child_audio.attach(child.data(), bytes, &child_error)) {
      _exit(4);
    }
    std::vector<float> frames(32 * channels);
    for (size_t f = 0; f < 32; ++f) {
      for (unsigned c = 0; c < channels; ++c) {
        frames[f * channels + c] = static_cast<float>(f);
      }
    }
    if (child_audio.to_host().write(frames.data(), 32) != 32) {
      _exit(5);
    }
    _exit(0);
  }

  std::vector<float> out(32 * channels, -1.0f);
  size_t got = 0;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
  while (got < 32 && std::chrono::steady_clock::now() < deadline) {
    const size_t n = audio.to_host().read(out.data() + got * channels, 32 - got);
    if (n == 0) {
      std::this_thread::yield();
      continue;
    }
    got += n;
  }

  int status = 0;
  waitpid(pid, &status, 0);
  CHECK_EQ(got, static_cast<size_t>(32));
  for (size_t f = 0; f < 32; ++f) {
    CHECK_EQ(out[f * channels], static_cast<float>(f));
  }
  CHECK(WIFEXITED(status));
  CHECK_EQ(WEXITSTATUS(status), 0);
  SharedRegion::unlink(name);
}

#endif