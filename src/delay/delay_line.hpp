#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "audio/backend.hpp"

namespace aes67_srt::delay {

/**
 * The A/V delay line: the audio side of lipsync (ticket 12, issue #13).
 *
 * Audio is the only side of the picture that can be delayed to match vision, so
 * this is ours to own, and it sits on the *egress* side — after the clock has
 * decided what to play and before the device plays it. The clock's playout level
 * is the delay the link forced on us (decision 6); this is the offset an operator
 * dials to line that audio up against someone else's vision (decision 8).
 *
 * **A dial is a jump, and a jump is a click.** The read head of a delay line
 * cannot teleport without a discontinuity, so every change of offset is a
 * *crossfade*: for the length of `fade_ms`, the old offset and the new one are
 * read at the same time and blended. At the window's ends the blend weight is
 * exactly 0 and 1, so the output is continuous with what came before and what
 * follows — which is the whole trick, and the thing the tests assert.
 *
 * **What a crossfade costs, stated rather than discovered.** During the window
 * the output is two copies of the programme offset by the change, so material
 * that changes fast across that offset blends into a short smear. That is the
 * price of not clicking, and it is why the fade is as short as it can be while
 * still bounding the step. The alternative — duck to silence, jump, duck back —
 * is provably continuous for any step size but is a dip, and a dip is more
 * audible than a blend. The ticket names the crossfade; this is it.
 *
 * **An offset of zero is a memcpy.** The steady state does no arithmetic at all,
 * so a link asked for no A/V offset is bit-exact through this module and the
 * clock's own byte-exactness result is untouched.
 *
 * **Not thread-safe, and it does not need to be.** The receive loop is the only
 * thread that touches it, exactly as with the clock; a control surface changes
 * the offset from another thread later (tickets 13-14) and will need a lock or a
 * queue of its own, which is that ticket's decision rather than this one's.
 */
class DelayLine {
 public:
  struct Config {
    unsigned channels = 64;
    unsigned sample_rate = 48000;
    unsigned period_frames = 48;
    unsigned sample_bytes = 3;  // s24_3le, or 2 for s16_le
    /**
     * The largest offset this line can hold.
     *
     * Spec open item 7 has not decided the product's range (0-2 s and 0-5 s are
     * different products), so this is the module's ceiling rather than a claim:
     * the engine sizes it from the same 5000 ms the configuration validator
     * accepts, and when the open item is answered this is the number that moves.
     */
    double capacity_ms = 5000.0;
    /** The crossfade window. Shorter steps less of the programme, and clicks. */
    double fade_ms = 10.0;
  };

  DelayLine();
  ~DelayLine();

  DelayLine(const DelayLine&) = delete;
  DelayLine& operator=(const DelayLine&) = delete;

  /** Allocate the line for |config|. Refuses an impossible geometry. */
  bool open(const Config& config, std::string* error);
  void close();
  bool is_open() const;

  /**
   * Ask for a new offset, in milliseconds. Refuses one outside 0..capacity.
   *
   * The read head heads for it over the fade window rather than jumping. A
   * request that arrives mid-fade is remembered and applied when the current
   * fade finishes, so a dial that is turned quickly settles on its final value
   * without ever stepping the output.
   */
  bool set_offset_ms(double offset_ms, std::string* error);

  /** The offset the last accepted request asked for. */
  double offset_ms() const;

  /** The offset the line is reading from right now, which trails `offset_ms`. */
  double applied_offset_ms() const;

  /** True while a crossfade is in progress. */
  bool adjusting() const;

  /**
   * Push one period and produce the delayed one.
   *
   * |input| and |output| are interleaved device samples; |frames| is one period.
   * They may be the same buffer. Until the line has been fed the offset it is
   * being asked for, the head of the output is silence — a delay has nothing to
   * say before it has heard anything.
   */
  bool process(const uint8_t* input, uint8_t* output, unsigned frames,
               std::string* error);

  /** Frames pushed since open(), for the ledger and the tests. */
  uint64_t pushed_frames() const;

  /** Crossfades completed since open(), so a test can prove one happened. */
  uint64_t crossfades() const;

 private:
  /** A weight that is exactly 0 at `step` 0 and exactly 1 at `step` |fade|. */
  double fade_weight(unsigned step) const;

  /** Copy |frames| frames starting at absolute frame |first| into |out|. */
  void copy_out(int64_t first, unsigned frames, uint8_t* out) const;

  /** One interleaved sample at an absolute frame, sign-extended. */
  int32_t sample_at(uint64_t frame, unsigned channel) const;

  Config config_;
  unsigned sample_bytes_ = 3;
  unsigned channels_ = 0;
  uint64_t ring_frames_ = 0;       // frames the ring holds
  uint64_t max_delay_frames_ = 0;  // the largest offset it will accept
  unsigned fade_frames_ = 0;       // the crossfade window, in frames
  std::vector<uint8_t> ring_;      // interleaved, ring_frames_ * channels
  uint64_t written_frames_ = 0;    // absolute frames pushed since open()
  uint64_t applied_delay_ = 0;     // the offset being read, in frames
  uint64_t target_delay_ = 0;      // the offset last asked for, in frames
  bool fading_ = false;
  uint64_t fade_from_ = 0;
  uint64_t fade_to_ = 0;
  unsigned fade_step_ = 0;
  uint64_t pushed_frames_ = 0;
  uint64_t crossfades_ = 0;
};

}  // namespace aes67_srt::delay
