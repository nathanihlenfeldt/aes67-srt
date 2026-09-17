#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "audio/backend.hpp"

namespace aes67_srt::delay {

/**
 * The impulse test signal: something to line audio up against vision with.
 *
 * An operator points a camera at a clap, or watches a flash, and needs a known
 * mark in the audio to match it to. This is the mark. It is an **impulse** — one
 * full-scale sample on one channel — because that is the most sample-accurate and
 * the most repeatable thing the audio path can carry: there is no waveform to get
 * wrong, and no ambiguity about *when* it happened.
 *
 * **Sample-accurate, and that is a promise about a frame index.** A trigger names
 * the absolute egress frame it lands on, and it lands on exactly that frame or the
 * trigger is refused. The engine counts egress frames as it writes them, so a mark
 * fired at the start of a run and a mark fired an hour in are placed the same way.
 *
 * **Repeatable, and that is a promise about bytes.** The impulse *replaces* the
 * sample on its channel rather than adding to it, so where it lands is exactly
 * full scale whatever was playing underneath — the same trigger through the same
 * line produces the same bytes, which is what makes it usable as a reference and
 * testable in CI. The cost is that it is a click, which is the point of it.
 *
 * **Not on the wire.** This is an egress signal — it is mixed in after the clock
 * and before the device, so it is heard at this end (and, in a duplex link, also
 * sent onward through the return path's own processing). A far-end alignment is
 * the far end's trigger, which is the only way it can know its own latency.
 */
class TestSignal {
 public:
  struct Config {
    unsigned channels = 64;
    unsigned sample_rate = 48000;
    unsigned period_frames = 48;
    unsigned sample_bytes = 3;
    /** The channel a trigger fires on; -1 turns the signal off. */
    int channel = -1;
    /**
     * How many triggers may be queued ahead of their frame. Eight is far more
     * than an alignment session needs and keeps the queue a fixed cost; a ninth
     * trigger is refused rather than silently growing.
     */
    unsigned max_pending = 8;
  };

  TestSignal();
  ~TestSignal();

  TestSignal(const TestSignal&) = delete;
  TestSignal& operator=(const TestSignal&) = delete;

  bool open(const Config& config, std::string* error);
  void close();
  bool is_open() const;

  /** True when a channel is selected, i.e. the signal is armed at all. */
  bool enabled() const;

  /** The selected channel, or -1. */
  int channel() const;

  /**
   * Queue an impulse for absolute egress frame |at_frame| on the selected
   * channel. A frame already passed, or a full queue, is refused rather than
   * landing in the wrong place.
   */
  bool trigger(uint64_t at_frame, std::string* error);

  /** The same, on a named channel, for a caller that wants a different one. */
  bool trigger(uint64_t at_frame, int channel, std::string* error);

  /**
   * Put any queued impulse that falls inside this period into |period|.
   *
   * |first_frame| is the absolute egress frame of the period's first frame, which
   * is what makes the placement sample-accurate regardless of where the period
   * boundaries happen to fall. An impulse that lands outside the period is left
   * queued for the period it belongs to.
   */
  void mix(uint8_t* period, unsigned frames, uint64_t first_frame);

  /** Triggers queued and not yet placed. */
  unsigned pending() const;

  /** Impulses placed since open(). */
  uint64_t fired() const;

  /** Triggers refused because the queue was full, since open(). */
  uint64_t missed() const;

 private:
  struct Pending {
    uint64_t frame = 0;
    int channel = 0;
  };

  Config config_;
  bool open_ = false;
  std::vector<Pending> queue_;
  uint64_t fired_ = 0;
  uint64_t missed_ = 0;
};

}  // namespace aes67_srt::delay
