// The realtime ring (ADR 0005): the boundary between a CoreAudio callback and the
// engine. These tests need no device, which is the point — the ring's discipline is
// provable before any hardware exists.

#include "audio/float_ring.hpp"

#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

#include "test_framework.hpp"

namespace {

using aes67_srt::audio::FloatRing;

}  // namespace

TEST_CASE(float_ring_carries_what_is_written_in_order) {
  FloatRing ring;
  CHECK(ring.open(16, 2));
  const std::vector<float> source = {1, 2, 3, 4, 5, 6};
  CHECK_EQ(ring.write(source.data(), 3), static_cast<size_t>(3));
  CHECK_EQ(ring.available_read(), static_cast<size_t>(3));

  std::vector<float> out(6, 0.0f);
  CHECK_EQ(ring.read(out.data(), 3), static_cast<size_t>(3));
  CHECK(out == source);
  CHECK_EQ(ring.available_read(), static_cast<size_t>(0));
}

TEST_CASE(float_ring_wraps_without_losing_order) {
  FloatRing ring;
  CHECK(ring.open(4, 1));
  // Push the read and write heads past the end of the storage several times.
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

TEST_CASE(float_ring_a_flood_is_truncated_and_counted_not_hidden) {
  FloatRing ring;
  CHECK(ring.open(8, 1));
  const std::vector<float> source(20, 1.0f);
  // Only eight frames fit; a realtime callback cannot wait for room.
  CHECK_EQ(ring.write(source.data(), 20), static_cast<size_t>(8));
  CHECK_EQ(ring.frames_dropped(), static_cast<uint64_t>(12));
  CHECK_EQ(ring.available_read(), static_cast<size_t>(8));
  CHECK_EQ(ring.frames_written(), static_cast<uint64_t>(8));
}

TEST_CASE(float_ring_a_starved_read_returns_short_and_counts_the_gap) {
  FloatRing ring;
  CHECK(ring.open(8, 1));
  const std::vector<float> source(2, 1.0f);
  CHECK_EQ(ring.write(source.data(), 2), static_cast<size_t>(2));
  std::vector<float> out(8, 0.0f);
  CHECK_EQ(ring.read(out.data(), 8), static_cast<size_t>(2));
  CHECK_EQ(ring.frames_underrun(), static_cast<uint64_t>(6));
}

TEST_CASE(float_ring_interleaves_channels_the_way_the_device_expects) {
  FloatRing ring;
  CHECK(ring.open(8, 3));
  // Two frames, three channels: L R C L R C.
  const std::vector<float> source = {1, 2, 3, 4, 5, 6};
  CHECK_EQ(ring.write(source.data(), 2), static_cast<size_t>(2));
  std::vector<float> out(6, 0.0f);
  CHECK_EQ(ring.read(out.data(), 2), static_cast<size_t>(2));
  CHECK(out == source);
}

TEST_CASE(float_ring_refuses_a_size_that_is_not_a_ring) {
  FloatRing ring;
  CHECK(!ring.open(1, 2));   // full and empty would be the same state
  CHECK(!ring.open(16, 0));  // no channels
  CHECK(!ring.is_open());
}

TEST_CASE(float_ring_two_threads_never_lose_or_reorder_a_frame) {
  // The discipline the realtime boundary rests on: one producer, one consumer, no
  // lock, and every frame that is *accepted* comes out in order and unchanged. The
  // producer retries when the ring is full (a test may wait; a callback may not),
  // so nothing is dropped and the sequence can be checked end to end.
  FloatRing ring;
  CHECK(ring.open(512, 2));
  const size_t total = 200000;
  std::atomic<bool> done{false};

  std::thread producer([&ring, total, &done] {
    std::vector<float> batch(64 * 2);
    size_t frame = 0;
    while (frame < total) {
      // Offer only what fits. Handing the ring more than it has room for is the
      // flood case, which drops and counts; here the producer wants every frame
      // delivered, so it waits for room instead (a test may wait; a callback may
      // not).
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
