#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "audio/backend.hpp"

namespace aes67_srt::audio {

/**
 * A device that needs no hardware, and that loops back.
 *
 * What is written to it is what it reads back, so a test can drive the whole
 * audio path — capture, framing, transmission, reassembly, playout — with no
 * ALSA device, no daemon and no network. That is how the commissioning loopback
 * is proved on a machine that has none of the three.
 *
 * It is not a mock: it is a backend with defined behaviour. A read with nothing
 * written pads with silence and counts an overrun, exactly as a real device would
 * starve, so tests can see the arithmetic rather than hope about it.
 *
 * **It also ticks at its nominal rate.** A period's worth of wall time passes
 * between successive reads and successive writes, because a real device paces its
 * caller and the engine is built on being paced by the device. This was not
 * obvious until it was run: without it, fake mode's transmit loop produced frames
 * thousands of times faster than realtime and dropped audio on the floor of the
 * loopback's queue within seconds.
 */
class NullBackend : public AudioBackend {
 public:
  NullBackend() = default;

  bool open(const AudioFormat& format, std::string* error) override;
  void close() override;
  bool is_open() const override;

  bool read(uint8_t* destination, unsigned frames, std::string* error) override;
  bool write(const uint8_t* source, unsigned frames, std::string* error) override;

  std::string kind() const override;
  std::string detail() const override;
  const AudioFormat& format() const override;

  unsigned overruns() const override;
  unsigned underruns() const override;

  /** Samples captured but not yet read: one period at most. */
  size_t pending_bytes() const;

 private:
  /**
   * Wait until this direction's next period is due. Not under the lock, and not
   * the same schedule for both directions: each thread owns its own tick, so
   * neither waits for the other's period to elapse.
   */
  void pace(std::chrono::steady_clock::time_point* next) const;

  bool open_ = false;
  AudioFormat format_;
  std::vector<uint8_t> loopback_;  // holds at most one period
  size_t pending_ = 0;             // bytes of loopback_ actually carrying audio

  /**
   * The two directions share one buffer by construction — that is what a
   * loopback is — so the device has to make the sharing safe. The engine calls
   * `read()` from one thread and `write()` from another, which the interface
   * allows and a real device handles by having physically separate capture and
   * playback paths.
   */
  mutable std::mutex mutex_;

  /** A period's duration, and when each direction's next one is due. */
  std::chrono::steady_clock::duration period_{};
  std::chrono::steady_clock::time_point next_read_tick_{};
  std::chrono::steady_clock::time_point next_write_tick_{};

  /**
   * Atomic because the audio thread increments them and the status page reads
   * them. The interface's "one thread at a time" governs read/write/open/close;
   * a counter is read from somewhere else, and a torn read of a plain `unsigned`
   * is undefined behaviour even where it happens to work.
   */
  std::atomic<unsigned> overruns_{0};
  std::atomic<unsigned> underruns_{0};
};

}  // namespace aes67_srt::audio
