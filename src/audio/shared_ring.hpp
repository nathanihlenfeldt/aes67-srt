#pragma once

// The shared-memory contract between the macOS endpoint's application and its HAL
// plug-in (ADR 0007, spec 0002). The plug-in runs in a sandboxed host process and
// is forbidden from blocking, allocating or locking, so it cannot call the engine
// directly; the two halves meet through a mapping both attach to.
//
// This header is deliberately free of CoreAudio and of any platform header, so it
// compiles and its tests run on Linux and macOS CI alike. Nothing here installs a
// driver or needs one: the ring's discipline is provable before the driver exists,
// which is the same bargain `audio::FloatRing` struck for ADR 0005.
//
// ## What one half sees
//
// One region holds two independent rings of interleaved 32-bit float:
//
//   - `to_host`  — the application produces, the plug-in's input callback consumes.
//                  This is the site's audio, on its way to the DAW.
//   - `from_host` — the plug-in's output callback produces (what the DAW played),
//                  the application consumes. This is the return path.
//
// Each ring is single-producer/single-consumer, and that is the whole contract: a
// second writer is a data race, not a slower ring. The indices are free-running
// frame counters, so full and empty stay distinguishable without wasting a slot and
// the arithmetic survives counter wrap.
//
// ## The edge, and why the two rings are not symmetric
//
// A realtime callback may neither wait for room nor invent samples, so an edge has
// to be answered rather than avoided:
//
//   - **The realtime producer overwrites the oldest.** When the plug-in's output
//     callback finds `from_host` full, it writes over the frames the application
//     has not yet read rather than discarding what it is holding now. Keeping the
//     newest keeps the send path near live; the frames lost are counted in
//     `frames_dropped`. (The application, which may wait, checks `available_write`
//     and does not overwrite, so in steady state this never happens.)
//   - **The realtime consumer reads short and pads.** When the plug-in's input
//     callback finds `to_host` empty it takes what is there and fills the rest with
//     silence, counting the gap in `frames_underrun`. A hole is kinder than a
//     repeat, which buzzes on tonal material.
//
// This is deliberately **not** `FloatRing`'s policy. `FloatRing` truncates a full
// ring, which drops the *newest* frames; on the realtime producer that keeps stale
// audio and lets the send path drift away from live. The lock-free shape, the
// atomics and the flood/starve tests are the same; only the full-ring rule differs.
//
// ## Memory ordering
//
// Each index is written by exactly one side, so each side may load its own index
// `relaxed` and must load the other's with `acquire` before touching the storage
// that index releases. The producer stores its index `release` **after** copying
// the samples, so a consumer that sees the index sees the samples. This is the same
// single-writer-per-index argument `FloatRing` documents.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

namespace aes67_srt::audio {

/** Marks a region as one of ours. A foreign or uninitialized region is refused. */
constexpr uint32_t kSharedRingMagic = 0x41363752u;  // "A67R"

/**
 * The layout version. Bump it whenever the fields or their order change, so an old
 * application and a new plug-in refuse each other by name instead of reading each
 * other's bytes as a ring.
 */
constexpr uint32_t kSharedRingVersion = 1;

/** Per-ring indices and counters, placed in the shared region. */
struct alignas(64) SharedRingState {
  std::atomic<uint64_t> write_index{0};  // frames, free-running
  std::atomic<uint64_t> read_index{0};   // frames, free-running
  std::atomic<uint64_t> frames_written{0};
  std::atomic<uint64_t> frames_read{0};
  /** Unread frames the producer overwrote (the realtime producer's overrun). */
  std::atomic<uint64_t> frames_dropped{0};
  /** Frames a read wanted but the ring did not hold (the realtime consumer's gap).
   */
  std::atomic<uint64_t> frames_underrun{0};
};

/**
 * The region header. The two float buffers follow it in the same mapping; their
 * offsets are computed from this struct's size, never stored as pointers, because a
 * pointer written by one process is meaningless in another.
 */
struct alignas(64) SharedBlock {
  uint32_t magic = 0;
  uint32_t version = 0;
  uint32_t channels = 0;
  uint32_t capacity_frames = 0;
  SharedRingState to_host;
  SharedRingState from_host;
};

/** The two float buffers begin here, past the header, rounded up to a cache line.
 */
size_t shared_data_offset();
/** Bytes one region needs for `capacity_frames` frames of `channels` channels. */
size_t shared_bytes_for(size_t capacity_frames, unsigned channels);

/**
 * A single-producer/single-consumer ring over storage the caller owns — a shared
 * mapping in production, a heap buffer in a test. It holds no storage of its own,
 * so the same class runs in-process and across a process boundary.
 *
 * `attach` takes the state and the buffer separately: in a region they come from
 * `SharedAudio`, but a test may point them at a plain array.
 */
class SharedRing {
 public:
  SharedRing() = default;
  ~SharedRing() = default;
  SharedRing(const SharedRing&) = delete;
  SharedRing& operator=(const SharedRing&) = delete;

  void attach(float* data, size_t capacity_frames, unsigned channels,
              SharedRingState* state);
  void detach();

  bool is_attached() const { return data_ != nullptr && state_ != nullptr; }
  size_t capacity_frames() const { return capacity_frames_; }
  unsigned channels() const { return channels_; }

  /**
   * Producer: write up to `frames`. Returns what was accepted, which is `frames`
   * unless it exceeds the whole ring. **Never blocks**, and overwrites the oldest
   * unread frames when the ring is full — the realtime producer's rule.
   */
  size_t write(const float* source, size_t frames);

  /**
   * Consumer: read up to `frames`. Returns what was there — short when starved, and
   * short when the producer has lapped and the oldest frames are gone. **Never
   * blocks**; the caller decides what to do about the gap (the callback pads).
   */
  size_t read(float* destination, size_t frames);

  size_t available_read() const;
  size_t available_write() const;
  /** True when the producer has overwritten frames the consumer has not read. */
  bool lapped() const;

  uint64_t frames_written() const;
  uint64_t frames_read() const;
  uint64_t frames_dropped() const;
  uint64_t frames_underrun() const;

 private:
  float* data_ = nullptr;
  size_t capacity_frames_ = 0;
  unsigned channels_ = 0;
  SharedRingState* state_ = nullptr;
};

/**
 * The whole contract: one region, two rings, and the version check that makes a
 * mismatched pair refuse each other by name.
 */
class SharedAudio {
 public:
  SharedAudio() = default;
  ~SharedAudio() = default;
  SharedAudio(const SharedAudio&) = delete;
  SharedAudio& operator=(const SharedAudio&) = delete;

  /**
   * Initialize a fresh region. Called once, by whichever side creates the mapping.
   * Returns false and sets `error` on a region too small or impossible dimensions.
   */
  bool create(void* region, size_t bytes, size_t capacity_frames, unsigned channels,
              std::string* error);

  /**
   * Attach to a region another side created. Refuses a foreign region, a version it
   * does not implement, or an impossible shape — naming which, because "the driver
   * is the wrong version" and "that memory is not ours" are different fixes.
   */
  bool attach(void* region, size_t bytes, std::string* error);

  SharedRing& to_host() { return to_host_; }
  SharedRing& from_host() { return from_host_; }
  const SharedBlock* block() const { return block_; }

 private:
  bool bind(void* region, size_t bytes, std::string* error);

  SharedBlock* block_ = nullptr;
  SharedRing to_host_;
  SharedRing from_host_;
};

}  // namespace aes67_srt::audio
