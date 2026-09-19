// The plug-in's realtime copy logic (issue #37): the body of the two libASPL I/O
// callbacks, proved on any platform — including that it allocates nothing, which is
// the discipline the realtime thread cannot be trusted to keep without a test.

#include "audio/device_bridge.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <new>
#include <vector>

#include "audio/shared_ring.hpp"
#include "test_framework.hpp"

namespace {

using aes67_srt::audio::DeviceBridge;
using aes67_srt::audio::SharedRing;
using aes67_srt::audio::SharedRingState;

// --- allocation sentinel -----------------------------------------------------
// Global `operator new` is replaceable, so this file's override is the program's
// for the test binary. It counts only while armed and always forwards to the C
// allocator, so the rest of the suite is unaffected.
std::atomic<uint64_t> g_allocations{0};
std::atomic<bool> g_armed{false};

void maybe_count() {
  if (g_armed.load(std::memory_order_relaxed)) {
    g_allocations.fetch_add(1, std::memory_order_relaxed);
  }
}

void* allocate(size_t size) {
  void* p = std::malloc(size == 0 ? 1 : size);
  if (p == nullptr) {
    throw std::bad_alloc();
  }
  return p;
}

// A bound pair of rings over caller-owned storage, the way the driver binds them.
struct TwoRings {
  std::vector<float> to_host_storage;
  std::vector<float> from_host_storage;
  SharedRingState to_host_state;
  SharedRingState from_host_state;
  SharedRing to_host;
  SharedRing from_host;

  TwoRings(size_t capacity, unsigned channels)
      : to_host_storage(capacity * channels, 0.0f),
        from_host_storage(capacity * channels, 0.0f) {
    to_host.attach(to_host_storage.data(), capacity, channels, &to_host_state);
    from_host.attach(from_host_storage.data(), capacity, channels,
                     &from_host_state);
  }
};

}  // namespace

void* operator new(std::size_t size) {
  maybe_count();
  return allocate(size);
}
void* operator new[](std::size_t size) {
  maybe_count();
  return allocate(size);
}
void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
  maybe_count();
  return std::malloc(size == 0 ? 1 : size);
}
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
  maybe_count();
  return std::malloc(size == 0 ? 1 : size);
}
void* operator new(std::size_t size, std::align_val_t align) {
  maybe_count();
  void* p = nullptr;
  if (posix_memalign(&p, static_cast<size_t>(align), size == 0 ? 1 : size) != 0) {
    throw std::bad_alloc();
  }
  return p;
}
void* operator new[](std::size_t size, std::align_val_t align) {
  maybe_count();
  void* p = nullptr;
  if (posix_memalign(&p, static_cast<size_t>(align), size == 0 ? 1 : size) != 0) {
    throw std::bad_alloc();
  }
  return p;
}
void operator delete(void* p, std::align_val_t) noexcept {
  std::free(p);
}
void operator delete[](void* p, std::align_val_t) noexcept {
  std::free(p);
}
void operator delete(void* p) noexcept {
  std::free(p);
}
void operator delete[](void* p) noexcept {
  std::free(p);
}
void operator delete(void* p, std::size_t) noexcept {
  std::free(p);
}
void operator delete[](void* p, std::size_t) noexcept {
  std::free(p);
}
void operator delete(void* p, const std::nothrow_t&) noexcept {
  std::free(p);
}
void operator delete[](void* p, const std::nothrow_t&) noexcept {
  std::free(p);
}

TEST_CASE(device_bridge_copies_received_audio_to_the_input_callback) {
  TwoRings rings(32, 2);
  const std::vector<float> source = {1, 2, 3, 4, 5, 6};
  CHECK_EQ(rings.to_host.write(source.data(), 3), static_cast<size_t>(3));

  DeviceBridge bridge;
  bridge.bind(&rings.to_host, &rings.from_host);
  CHECK(bridge.bound());

  std::vector<float> out(6, -1.0f);
  CHECK_EQ(bridge.read_input(out.data(), 3), static_cast<size_t>(3));
  CHECK(out == source);
  CHECK_EQ(bridge.frames_silenced(), static_cast<uint64_t>(0));
}

TEST_CASE(device_bridge_pads_a_starved_input_with_silence) {
  TwoRings rings(32, 2);
  const std::vector<float> source = {7, 7};
  CHECK_EQ(rings.to_host.write(source.data(), 1), static_cast<size_t>(1));

  DeviceBridge bridge;
  bridge.bind(&rings.to_host, &rings.from_host);

  // The callback must always fill its buffer, so the tail is silence.
  std::vector<float> out(6, -1.0f);
  CHECK_EQ(bridge.read_input(out.data(), 3), static_cast<size_t>(1));
  CHECK_EQ(out[0], 7.0f);
  CHECK_EQ(out[1], 7.0f);
  CHECK_EQ(out[2], 0.0f);
  CHECK_EQ(out[3], 0.0f);
  CHECK_EQ(out[4], 0.0f);
  CHECK_EQ(out[5], 0.0f);
  CHECK_EQ(bridge.frames_silenced(), static_cast<uint64_t>(2));
}

TEST_CASE(device_bridge_takes_what_the_client_played) {
  TwoRings rings(32, 2);
  DeviceBridge bridge;
  bridge.bind(&rings.to_host, &rings.from_host);

  const std::vector<float> played = {1, 2, 3, 4, 5, 6};
  CHECK_EQ(bridge.write_output(played.data(), 3), static_cast<size_t>(3));

  std::vector<float> out(6, 0.0f);
  CHECK_EQ(rings.from_host.read(out.data(), 3), static_cast<size_t>(3));
  CHECK(out == played);
}

TEST_CASE(device_bridge_overwrites_the_oldest_on_the_output_when_full) {
  // The realtime producer's rule, reached through the callback's body.
  TwoRings rings(8, 1);
  DeviceBridge bridge;
  bridge.bind(&rings.to_host, &rings.from_host);

  std::vector<float> first = {0, 1, 2, 3, 4, 5, 6, 7};
  CHECK_EQ(bridge.write_output(first.data(), 8), static_cast<size_t>(8));
  std::vector<float> second = {8, 9, 10, 11};
  CHECK_EQ(bridge.write_output(second.data(), 4), static_cast<size_t>(4));
  CHECK_EQ(rings.from_host.frames_dropped(), static_cast<uint64_t>(4));

  std::vector<float> out(8, -1.0f);
  CHECK_EQ(rings.from_host.read(out.data(), 8), static_cast<size_t>(8));
  for (size_t i = 0; i < out.size(); ++i) {
    CHECK_EQ(out[i], static_cast<float>(4 + i));
  }
}

TEST_CASE(device_bridge_an_unbound_bridge_is_silence_not_a_crash) {
  // The region may not exist yet when the device's I/O starts. The device must stay
  // present and the DAW must get defined audio, not a fault.
  DeviceBridge bridge;
  bridge.configure(2);  // known from the stream format, no region required
  CHECK(!bridge.bound());

  std::vector<float> out(6, -1.0f);
  CHECK_EQ(bridge.read_input(out.data(), 3), static_cast<size_t>(0));
  for (float sample : out) {
    CHECK_EQ(sample, 0.0f);
  }
  const std::vector<float> played = {1, 2, 3};
  CHECK_EQ(bridge.write_output(played.data(), 3), static_cast<size_t>(0));
}

TEST_CASE(device_bridge_the_realtime_path_allocates_nothing) {
  TwoRings rings(256, 4);
  DeviceBridge bridge;
  bridge.bind(&rings.to_host, &rings.from_host);

  std::vector<float> audio(256 * 4, 1.0f);
  std::vector<float> out(64 * 4, 0.0f);
  CHECK_EQ(rings.to_host.write(audio.data(), 64), static_cast<size_t>(64));

  // Warm the path once, then count. Everything the buffer needs is already
  // allocated, so a callback that allocates is a callback that allocates on every
  // call.
  bridge.read_input(out.data(), 64);
  bridge.write_output(out.data(), 64);

  g_allocations.store(0, std::memory_order_relaxed);
  g_armed.store(true, std::memory_order_relaxed);
  for (int i = 0; i < 100; ++i) {
    const size_t read = bridge.read_input(out.data(), 64);
    (void)read;
    bridge.write_output(out.data(), 64);
  }
  g_armed.store(false, std::memory_order_relaxed);

  CHECK_EQ(g_allocations.load(std::memory_order_relaxed), static_cast<uint64_t>(0));
}