#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "audio/backend.hpp"
#include "audio/pcm.hpp"
#include "clock/playout_buffer.hpp"

namespace aes67_srt::clock {

/**
 * The resampler: ADR 0003's continuous conversion, at the ratio the loop asks for.
 *
 * This is increment 3 of the clock module (ticket 11, issue #12), and the last
 * piece of it: the buffer holds the sender's audio, the ratio control decides how
 * fast it must be consumed, and this is what consumes it — turning the sender's
 * 48 kHz into *our* 48 kHz, continuously, so that there is never a correction to
 * hear. The argument for that is ADR 0003's; what is here is the mechanism.
 *
 * **The ratio's direction is the one thing that cannot be got wrong quietly.**
 * libsamplerate defines `src_ratio` as **output sample rate / input sample rate**
 * (`libsndfile.github.io/libsamplerate/api_misc.html`, read rather than
 * remembered), and the ratio control produces the reciprocal: *input frames
 * consumed per output frame*, because consuming at the sender's rate is what holds
 * the buffer level still. So `src_ratio = 1 / control.ratio()`, and an error here
 * is a loop that runs away rather than one that corrects. The tests assert the
 * consumption rate for exactly that reason.
 *
 * **No step in the ratio, deliberately.** The library interpolates between the
 * ratio of one call and the next, and `src_set_ratio()` exists to bypass that and
 * give a step response. We do not call it: a step in the ratio is a step in the
 * pitch of everything playing, and the control's own rate limit is what decides
 * how fast the ratio may move.
 *
 * **What it costs the audio.** The bytes become floats here and only here, because
 * that is what the converter takes. The conversion is lossless in both directions —
 * every 24-bit integer is exactly representable in a float, and the way back
 * rounds to nearest and clips rather than wrapping — so the only thing that
 * touches the samples is the converter's own filter, which is the point of it.
 */
class Resampler {
 public:
  /**
   * Which of libsamplerate's converters to use, named as its documentation names
   * them.
   *
   * All three sinc converters are documented at 97 dB SNR; what the CPU buys is
   * **bandwidth** — 97%, 90% and 80% of Nyquist — which at 48 kHz is the
   * difference between a passband that reaches 23.3 kHz, one that reaches 21.6
   * kHz, and one that stops at 19.2 kHz and takes the top of the audible band with
   * it. The default is measured rather than assumed; see
   * `docs/research/clock-recovery.md`.
   */
  enum class Converter { sinc_best, sinc_medium, sinc_fastest };

  struct Config {
    Converter converter = Converter::sinc_medium;
    unsigned channels = 64;
    unsigned rate = 48000;
    unsigned period_frames = 48;
  };

  Resampler();
  ~Resampler();

  Resampler(const Resampler&) = delete;
  Resampler& operator=(const Resampler&) = delete;

  /** Open a converter for |config|. Refuses when this build has no libsamplerate.
   */
  bool open(const Config& config, std::string* error);
  void close();
  bool is_open() const;

  /**
   * The ratio to apply: input frames consumed per output frame, which is what the
   * ratio control produces. 1.0 is two clocks that agree.
   */
  void set_ratio(double ratio);
  double ratio() const;

  /**
   * Produce |frames| frames of output for the device, pulling whole periods from
   * |buffer| for as long as the ratio says it needs them.
   *
   * Returns false when the buffer had nothing to give and nothing has been
   * produced: nothing is written, nothing is invented, and the caller decides what
   * the device hears instead — the same contract `PlayoutBuffer::take` has, because
   * this is the same decision one layer up.
   *
   * When the buffer runs dry part-way through, the rest of |output| is silence and
   * the pull is counted in short_pulls(): the device is owed a whole period, and
   * there is no truthful way to give it less.
   *
   * |position| receives the sender's sample position corresponding to the first
   * frame of this output, which is what ticket 13's A/V offset needs to line audio
   * up against video. It may be null.
   */
  bool pull(PlayoutBuffer* buffer, uint8_t* output, unsigned frames,
            uint64_t* position, std::string* error);

  /** Frames consumed from the buffer and produced for the device, since open(). */
  uint64_t input_frames() const;
  uint64_t output_frames() const;

  /** Pulls that ran out of input and had to pad a period with silence. */
  uint64_t short_pulls() const;

  /**
   * Input the converter has taken and not yet produced, in frames.
   *
   * Measured, not documented, because a number here is what the A/V delay figure
   * has to account for. The library takes more input than it produces and keeps the
   * difference: **one to two periods (1–2 ms), depending on the ratio.** That is
   * audio received and not yet played. It is *not* a delay in the played audio —
   * the output of a frame corresponds to that same input frame, measured at zero
   * lag — it is working room, and the distinction is why this is a separate number
   * from `delay_frames()`.
   *
   * With the control's ratio moving, this ledger is approximate: it is the whole
   * input consumed since `open()` less the input the output produced should have
   * taken.
   */
  uint64_t held_frames() const;

  /**
   * Input frames taken from the buffer and not yet given to the converter.
   *
   * With `input_frames()`, this is a ledger that has to balance to the frame:
   * everything the buffer handed over was either consumed by the converter or is
   * still here. The test asserts that equation rather than trusting the carry,
   * because a frame dropped in the carry is audio lost with nothing in the log.
   */
  uint64_t pending_frames() const;

  /**
   * The delay the converter itself adds, in frames: how far the audio in an output
   * frame is behind the input position `pull` reports. Measured on an impulse by
   * the tests rather than taken from documentation, because ADR 0003 requires the
   * A/V delay line to account for the resampler's delay alongside the codec's, and
   * the documentation does not state it.
   */
  unsigned delay_frames() const;

  /** True when this build contains the resampler. */
  static bool available();

  /** Why not, in a sentence the log can carry. */
  static std::string unavailable_reason();

 private:
  /** Take one period from the buffer into the carry. False when there is none, or
   *  when the converter's look-ahead has outgrown the scratch. */
  bool fill_from_buffer(PlayoutBuffer* buffer, std::string* error);

  struct Impl;
  Impl* impl_;
};

/** The name as it appears in logs and configuration. */
const char* to_string(Resampler::Converter converter);

/** Parse "sinc_best" / "sinc_medium" / "sinc_fastest". False for anything else. */
bool parse_converter(const std::string& text, Resampler::Converter* converter);

/**
 * The byte seam now lives in `audio/pcm.hpp`, and these aliases keep the names
 * the clock has always used. It moved because a device backend needs the same
 * conversion, and a backend that reached into `clock` for it would point the
 * dependency the wrong way.
 */
using audio::float_to_s24_3le;
using audio::s24_3le_to_float;

}  // namespace aes67_srt::clock