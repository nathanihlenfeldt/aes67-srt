// The shared-memory contract between the plug-in and the application (ADR 0007,
// spec 0002, issue #36). Like the FloatRing tests, these need no device, no driver
// and no CoreAudio: the ring's discipline is provable first, in-process and across
// a real process boundary.

#include "audio/shared_ring.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "test_framework.hpp"

namespace {

using aes67_srt::audio::kSharedRingVersion;
using aes67_srt::audio::shared_bytes_for;
using aes67_srt::audio::SharedAudio;
using aes67_srt::audio::SharedBlock;
using aes67_srt::audio::SharedRing;
using aes67_srt::audio::SharedRingState;

// A heap region aligned the way a mapping is, for the create/attach tests.
struct AlignedRegion {
  void* data = nullptr;
  explicit AlignedRegion(size_t bytes) {
    if (posix_memalign(&data, 64, ((bytes + 63) / 64) * 64) != 0) {
      data = nullptr;
    }
  }
  ~AlignedRegion() { std::free(data); }
  void* get() const { return data; }
};

}  // namespace

TEST_CASE(shared_ring_carries_what_is_written_in_order) {
  std::vector<float> storage(16 * 2, 0.0f);
  SharedRingState state;
  SharedRing ring;
  ring.attach(storage.data(), 16, 2, &state);
  CHECK(ring.is_attached());

  const std::vector<float> source = {1, 2, 3, 4, 5, 6};
  CHECK_EQ(ring.write(source.data(), 3), static_cast<size_t>(3));
  CHECK_EQ(ring.available_read(), static_cast<size_t>(3));

  std::vector<float> out(6, 0.0f);
  CHECK_EQ(ring.read(out.data(), 3), static_cast<size_t>(3));
  CHECK(out == source);
  CHECK_EQ(ring.frames_written(), static_cast<uint64_t>(3));
  CHECK_EQ(ring.frames_read(), static_cast<uint64_t>(3));
  CHECK_EQ(ring.frames_dropped(), static_cast<uint64_t>(0));
  CHECK_EQ(ring.frames_underrun(), static_cast<uint64_t>(0));
}

TEST_CASE(shared_ring_wraps_without_losing_order) {
  std::vector<float> storage(4, 0.0f);
  SharedRingState state;
  SharedRing ring;
  ring.attach(storage.data(), 4, 1, &state);
  for (int round = 0; round < 10; ++round) {
    const float value = static_cast<float>(round);
    std::vector<float> source(3, value);
    CHECK_EQ(ring.write(source.data(), 3), static_cast<size_t>(3));
    std::vector<float> out(3, -1.0f);
    CHECK_EQ(ring.read(out.data(), 3), static_cast<size_t>(3));
    for (float sample : out) {
      CHECK_EQ(sample, value);
    }
  }
}

TEST_CASE(shared_ring_overwrites_the_oldest_and_counts_the_loss) {
  // The realtime producer's rule: a full ring gives up the frames the consumer has
  // not read, keeping the newest, and counts what it lost rather than hiding it.
  std::vector<float> storage(8, 0.0f);
  SharedRingState state;
  SharedRing ring;
  ring.attach(storage.data(), 8, 1, &state);

  std::vector<float> first = {0, 1, 2, 3, 4, 5, 6, 7};
  CHECK_EQ(ring.write(first.data(), 8), static_cast<size_t>(8));
  CHECK_EQ(ring.frames_dropped(), static_cast<uint64_t>(0));

  std::vector<float> second = {8, 9, 10, 11};
  CHECK_EQ(ring.write(second.data(), 4), static_cast<size_t>(4));
  CHECK_EQ(ring.frames_dropped(), static_cast<uint64_t>(4));
  CHECK(ring.lapped());

  // The newest eight survive: frames 4..11, not 0..7.
  std::vector<float> out(8, -1.0f);
  CHECK_EQ(ring.read(out.data(), 8), static_cast<size_t>(8));
  for (size_t i = 0; i < out.size(); ++i) {
    CHECK_EQ(out[i], static_cast<float>(4 + i));
  }
  CHECK(!ring.lapped());
}

TEST_CASE(shared_ring_an_offer_bigger_than_the_ring_keeps_the_newest) {
  std::vector<float> storage(8, 0.0f);
  SharedRingState state;
  SharedRing ring;
  ring.attach(storage.data(), 8, 1, &state);

  std::vector<float> source(20);
  for (size_t i = 0; i < source.size(); ++i) {
    source[i] = static_cast<float>(i);
  }
  CHECK_EQ(ring.write(source.data(), 20), static_cast<size_t>(8));
  CHECK_EQ(ring.frames_dropped(), static_cast<uint64_t>(12));

  std::vector<float> out(8, -1.0f);
  CHECK_EQ(ring.read(out.data(), 8), static_cast<size_t>(8));
  for (size_t i = 0; i < out.size(); ++i) {
    CHECK_EQ(out[i], static_cast<float>(12 + i));
  }
}

TEST_CASE(shared_ring_a_starved_read_returns_short_and_counts_the_gap) {
  // The realtime consumer's rule: take what is there and count the rest; the caller
  // (the input callback) fills the gap with silence.
  std::vector<float> storage(8, 0.0f);
  SharedRingState state;
  SharedRing ring;
  ring.attach(storage.data(), 8, 1, &state);

  std::vector<float> source = {7, 7};
  CHECK_EQ(ring.write(source.data(), 2), static_cast<size_t>(2));
  std::vector<float> out(8, 0.0f);
  CHECK_EQ(ring.read(out.data(), 8), static_cast<size_t>(2));
  CHECK_EQ(ring.frames_underrun(), static_cast<uint64_t>(6));
}

TEST_CASE(shared_ring_interleaves_channels_the_way_the_device_expects) {
  std::vector<float> storage(8 * 3, 0.0f);
  SharedRingState state;
  SharedRing ring;
  ring.attach(storage.data(), 8, 3, &state);
  const std::vector<float> source = {1, 2, 3, 4, 5, 6};
  CHECK_EQ(ring.write(source.data(), 2), static_cast<size_t>(2));
  std::vector<float> out(6, 0.0f);
  CHECK_EQ(ring.read(out.data(), 2), static_cast<size_t>(2));
  CHECK(out == source);
}

TEST_CASE(shared_ring_two_threads_never_lose_or_reorder_a_frame) {
  // The two-process test below is the real proof; this is the in-process twin, and
  // it is also the non-realtime producer's rule — it waits for room instead of
  // overwriting, so nothing is dropped.
  std::vector<float> storage(512 * 2, 0.0f);
  SharedRingState state;
  SharedRing ring;
  ring.attach(storage.data(), 512, 2, &state);

  const size_t total = 200000;
  std::atomic<bool> done{false};
  std::thread producer([&ring, &done] {
    std::vector<float> batch(64 * 2);
    size_t frame = 0;
    while (frame < total) {
      const size_t want = std::min<size_t>(64, total - frame);
      const size_t count = std::min(want, ring.available_write());
      if (count == 0) {
        std::this_thread::yield();
        continue;
      }
      for (size_t i = 0; i < count; ++i) {
        batch[i * 2] = static_cast<float>(frame + i);
        batch[i * 2 + 1] = static_cast<float>(frame + i);
      }
      CHECK_EQ(ring.write(batch.data(), count), count);
      frame += count;
    }
    done.store(true);
  });

  std::vector<float> out(64 * 2);
  int64_t expected = 0;
  while (!done.load() || ring.available_read() > 0) {
    const size_t got = ring.read(out.data(), 64);
    for (size_t i = 0; i < got; ++i) {
      CHECK_EQ(out[i * 2], static_cast<float>(expected));
      CHECK_EQ(out[i * 2 + 1], static_cast<float>(expected));
      ++expected;
    }
  }
  producer.join();
  CHECK_EQ(expected, static_cast<int64_t>(total));
  CHECK_EQ(ring.frames_dropped(), static_cast<uint64_t>(0));
}

TEST_CASE(shared_audio_a_second_attach_sees_the_same_rings) {
  const size_t bytes = shared_bytes_for(32, 2);
  AlignedRegion region(bytes);
  CHECK(region.get() != nullptr);
  if (region.get() == nullptr) {
    return;
  }

  SharedAudio creator;
  std::string error;
  CHECK(creator.create(region.get(), bytes, 32, 2, &error));
  CHECK(error.empty());

  SharedAudio second;
  CHECK(second.attach(region.get(), bytes, &error));
  CHECK(second.to_host().is_attached());

  // A frame written through one view is read through the other.
  const std::vector<float> source = {1, 2, 3, 4};
  CHECK_EQ(creator.to_host().write(source.data(), 2), static_cast<size_t>(2));
  std::vector<float> out(4, 0.0f);
  CHECK_EQ(second.to_host().read(out.data(), 2), static_cast<size_t>(2));
  CHECK(out == source);
}

TEST_CASE(shared_audio_refuses_a_region_that_is_not_ours) {
  const size_t bytes = shared_bytes_for(32, 2);
  AlignedRegion region(bytes);
  CHECK(region.get() != nullptr);
  if (region.get() == nullptr) {
    return;
  }
  SharedAudio creator;
  std::string error;
  CHECK(creator.create(region.get(), bytes, 32, 2, &error));
  auto* block = const_cast<SharedBlock*>(creator.block());
  block->magic = 0;  // someone else's memory

  SharedAudio audio;
  CHECK(!audio.attach(region.get(), bytes, &error));
  CHECK(error.find("magic") != std::string::npos);
}

TEST_CASE(shared_audio_refuses_a_version_it_does_not_speak) {
  const size_t bytes = shared_bytes_for(32, 2);
  AlignedRegion region(bytes);
  CHECK(region.get() != nullptr);
  if (region.get() == nullptr) {
    return;
  }
  SharedAudio creator;
  std::string error;
  CHECK(creator.create(region.get(), bytes, 32, 2, &error));
  auto* block = const_cast<SharedBlock*>(creator.block());
  block->version = kSharedRingVersion + 1;  // a future build's layout

  SharedAudio audio;
  CHECK(!audio.attach(region.get(), bytes, &error));
  CHECK(error.find("version") != std::string::npos);
}

TEST_CASE(shared_audio_refuses_a_region_too_small_for_its_shape) {
  const size_t bytes = shared_bytes_for(32, 2) - 8;
  AlignedRegion region(shared_bytes_for(32, 2));
  CHECK(region.get() != nullptr);
  if (region.get() == nullptr) {
    return;
  }
  SharedAudio creator;
  std::string error;
  CHECK(creator.create(region.get(), shared_bytes_for(32, 2), 32, 2, &error));

  SharedAudio audio;
  CHECK(!audio.attach(region.get(), bytes, &error));
  CHECK(error.find("too small") != std::string::npos);
}

#if defined(__unix__) || defined(__APPLE__)

TEST_CASE(shared_ring_carries_frames_between_two_processes) {
  // The cross-process proof: one process produces, another consumes, over a real
  // shared mapping, and every accepted frame comes out in order and unchanged. The
  // child reports only through its exit code, so no test output is duplicated.
  constexpr size_t capacity = 256;
  constexpr unsigned channels = 2;
  constexpr size_t total = 50000;
  const size_t bytes = shared_bytes_for(capacity, channels);
  void* region = mmap(nullptr, bytes, PROT_READ | PROT_WRITE,
                      MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  CHECK(region != MAP_FAILED);
  if (region == MAP_FAILED) {
    return;
  }

  SharedAudio audio;
  std::string error;
  CHECK(audio.create(region, bytes, capacity, channels, &error));

  const pid_t pid = fork();
  CHECK(pid >= 0);
  if (pid == 0) {
    // Child: the non-realtime producer. It waits for room, so nothing is dropped
    // and the parent can check the sequence end to end.
    std::vector<float> batch(32 * channels);
    size_t frame = 0;
    while (frame < total) {
      const size_t want = std::min<size_t>(32, total - frame);
      const size_t room = std::min(want, audio.to_host().available_write());
      if (room == 0) {
        std::this_thread::yield();
        continue;
      }
      for (size_t i = 0; i < room; ++i) {
        for (unsigned c = 0; c < channels; ++c) {
          batch[i * channels + c] = static_cast<float>(frame + i);
        }
      }
      if (audio.to_host().write(batch.data(), room) != room) {
        _exit(2);
      }
      frame += room;
    }
    _exit(0);
  }

  // Parent: the consumer, verifying order across the process boundary.
  std::vector<float> out(32 * channels);
  size_t expected = 0;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
  while (expected < total && std::chrono::steady_clock::now() < deadline) {
    const size_t got = audio.to_host().read(out.data(), 32);
    if (got == 0) {
      std::this_thread::yield();
      continue;
    }
    for (size_t i = 0; i < got; ++i) {
      for (unsigned c = 0; c < channels; ++c) {
        CHECK_EQ(out[i * channels + c], static_cast<float>(expected + i));
      }
    }
    expected += got;
  }

  int status = 0;
  waitpid(pid, &status, 0);
  CHECK_EQ(expected, total);
  CHECK(WIFEXITED(status));
  CHECK_EQ(WEXITSTATUS(status), 0);
  munmap(region, bytes);
}

#endif