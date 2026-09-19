#pragma once

// The plug-in's realtime logic, separated from libASPL and from CoreAudio so it can
// be tested on any platform and asserted allocation-free (ADR 0007, spec 0002,
// issue #37).
//
// The two libASPL I/O callbacks are thin: they hand this class the HAL's buffer and
// the frame count, and it does nothing but copy. Everything the callbacks must not
// do — allocate, lock, log, syscall — is therefore absent from one small class that
// a test can inspect, rather than buried in a bundle that only a real Mac can run.
//
// The edge rules are the spec's *Ring behaviour at an edge*:
//   - the input callback (what the DAW records) pads a starved read with silence;
//   - the output callback (what the DAW played) overwrites the oldest when full.
// Both fixes live in `SharedRing`; this class adds only the silence pad, which is
// the one thing a ring cannot do by itself (it has no buffer to write zeros into).

#include <cstddef>
#include <cstdint>

#include "audio/shared_ring.hpp"

namespace aes67_srt::audio {

/**
 * The body of the plug-in's two realtime callbacks, over a bound `SharedAudio`.
 *
 * It borrows the two rings; it owns no storage and allocates nothing after `bind`.
 * An unbound bridge is a valid state — the region may not exist yet when the
 * device's I/O starts — and answers with silence and a discard rather than a crash,
 * so the device stays present while the application is away.
 */
class DeviceBridge {
 public:
  DeviceBridge() = default;
  ~DeviceBridge() = default;
  DeviceBridge(const DeviceBridge&) = delete;
  DeviceBridge& operator=(const DeviceBridge&) = delete;

  /**
   * The device's channel count, known from its stream format whether or not the
   * region exists. It is what lets an unbound bridge still silence a starved read
   * instead of leaving the DAW's buffer undefined.
   */
  void configure(unsigned channels);
  unsigned channels() const { return channels_; }

  /** Borrow the two rings. Called before I/O starts, never from a callback. */
  void bind(SharedRing* to_host, SharedRing* from_host);
  void unbind();
  bool bound() const { return to_host_ != nullptr && from_host_ != nullptr; }

  /**
   * The input callback's body: fill `frames` frames for the client from the
   * received stream, padding with **silence** when the ring is starved. Always
   * writes `frames` frames, so the DAW's buffer is always defined. Returns how many
   * were real audio; `frames - returned` were silence.
   *
   * Realtime-safe: no allocation, no lock, no log. The pad is `memset`, and the
   * ring's own `frames_underrun` counter records the gap.
   */
  size_t read_input(float* destination, size_t frames);

  /**
   * The output callback's body: hand `frames` frames the client played to the
   * return-path ring, overwriting the oldest unread frames when it is full.
   * Returns the frames accepted (all of them, unless the offer exceeds the ring).
   *
   * Realtime-safe: no allocation, no lock, no log. The ring's `frames_dropped`
   * counter records any overwrite.
   */
  size_t write_output(const float* source, size_t frames);

  /** Frames this bridge has padded with silence since it was bound, for the page.
   */
  uint64_t frames_silenced() const {
    return frames_silenced_.load(std::memory_order_relaxed);
  }

 private:
  SharedRing* to_host_ = nullptr;
  SharedRing* from_host_ = nullptr;
  unsigned channels_ = 0;
  std::atomic<uint64_t> frames_silenced_{0};
};

}  // namespace aes67_srt::audio
