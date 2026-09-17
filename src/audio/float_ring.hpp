#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace aes67_srt::audio {

/**
 * A lock-free single-producer/single-consumer ring of interleaved float frames.
 *
 * This is the boundary ADR 0005 draws between a realtime audio callback and the
 * engine: the callback only ever calls `write` or `read` and updates an index, and
 * neither blocks, allocates, locks or logs. The engine's side calls the other one
 * from an ordinary thread, where blocking is allowed.
 *
 * **Exactly one producer and one consumer, and that is the whole contract.**
 * Two writers or two readers is a data race, not a slower ring. The indices are
 * free-running frame counters, so "full" and "empty" are distinguishable without
 * wasting a slot, and the arithmetic keeps working across counter wrap.
 *
 * **A full ring truncates the write and a starved ring returns short**, counting
 * both. Real-time audio cannot wait for room and cannot invent samples, so the
 * truthful answers are "here is what fitted" and "here is what was there"; the
 * counters make the difference visible to whoever is diagnosing it.
 */
class FloatRing {
 public:
  FloatRing() = default;
  ~FloatRing();

  FloatRing(const FloatRing&) = delete;
  FloatRing& operator=(const FloatRing&) = delete;

  /**
   * Size the ring. Allocates, so it must happen before either side is running —
   * never inside the callback. Refuses a ring too small to be useful.
   */
  bool open(size_t frames, unsigned channels);
  /** Release the storage. Only safe once both sides have stopped. */
  void close();
  bool is_open() const { return !buffer_.empty(); }

  size_t capacity_frames() const { return capacity_frames_; }
  unsigned channels() const { return channels_; }

  /**
   * Producer: write up to |frames| frames. Returns the frames accepted, which is
   * short when the ring is full. Never blocks.
   */
  size_t write(const float* source, size_t frames);

  /**
   * Consumer: read up to |frames| frames. Returns the frames available, which is
   * short when the ring is starved. Never blocks.
   */
  size_t read(float* destination, size_t frames);

  size_t available_read() const;
  size_t available_write() const;

  /** Totals since open, for the status page and for a test. */
  uint64_t frames_written() const { return frames_written_.load(); }
  uint64_t frames_read() const { return frames_read_.load(); }
  /** Frames a `write` had to drop because the ring was full. */
  uint64_t frames_dropped() const { return frames_dropped_.load(); }
  /** Frames a `read` wanted but the ring did not hold. */
  uint64_t frames_underrun() const { return frames_underrun_.load(); }

 private:
  std::vector<float> buffer_;
  size_t capacity_frames_ = 0;
  unsigned channels_ = 0;
  std::atomic<size_t> write_index_{0};  // frames, free-running
  std::atomic<size_t> read_index_{0};   // frames, free-running
  std::atomic<uint64_t> frames_written_{0};
  std::atomic<uint64_t> frames_read_{0};
  std::atomic<uint64_t> frames_dropped_{0};
  std::atomic<uint64_t> frames_underrun_{0};
};

}  // namespace aes67_srt::audio
