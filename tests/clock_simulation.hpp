#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "audio/backend.hpp"
#include "test_framework.hpp"

/**
 * The two-clock simulation the clock module's tests are built on.
 *
 * Both ends follow their own PTP grandmaster, so their sample clocks differ by
 * however many ppm their crystals differ. Everything the clock module does
 * follows from that one sentence, so there is one model of it rather than one per
 * test file: two `Pace`s, a period whose bytes name their own sample position,
 * and the arithmetic to read that back.
 *
 * **Integer-exact, deliberately.** A clock advances in millionths of a
 * millisecond, so a crystal offset of 10 ppm is exactly 10 units per millisecond
 * of wall time and four simulated hours accumulate no floating-point error. A
 * fractional model would make "no sample was lost over four hours" an assertion
 * about rounding.
 */
namespace clock_sim {

using aes67_srt::audio::AudioFormat;

/** One whole and one millionth, so a ppm offset is an integer count. */
constexpr uint64_t k_ppm = 1000000;

/** What three bytes of an L24 sample can carry: positions are compared modulo
 *  this, so the payload stays exactly as wide as the audio it describes. */
constexpr uint64_t k_tag_mask = 0xffffffu;

/**
 * One end's sample clock.
 *
 * `ppm` is its offset from the nominal 48 kHz, and `advance()` answers how many
 * of *its own* milliseconds passed during one of the wall clock's: one normally,
 * and two when the crystal is fast enough to fit an extra period in. At +10 ppm
 * that happens once in a hundred thousand milliseconds — one extra period (48
 * frames) per 100 seconds, which is 0.48 samples a second: the figure the whole
 * design turns on. A slow side is the other Pace in the simulation rather than a
 * negative ppm, because that is what "faster" means for one end of a link.
 */
struct Pace {
  uint64_t ppm = 0;
  uint64_t phase = 0;  // millionths of a millisecond

  uint64_t advance() {
    phase += k_ppm + ppm;
    const uint64_t whole = phase / k_ppm;
    phase -= whole * k_ppm;
    return whole;
  }
};

/**
 * The shape the appliance runs, minus the channels.
 *
 * Channels are what a period costs in bytes and what the wire's blocks are about;
 * nothing in the clock's arithmetic depends on them, and four simulated hours of
 * the real 9216-byte period is 40 GB of copying for no extra proof. The 64-channel
 * shape is exercised where it belongs instead: on one period, in the test that
 * asserts all eight blocks share a sample position.
 */
inline AudioFormat sim_format(unsigned channels = 8) {
  AudioFormat format;
  format.sample_rate = 48000;
  format.channels = channels;
  format.period_frames = 48;  // 1 ms at 48 kHz, AES67's ptime
  format.sample_bytes = 3;    // s24_3le
  return format;
}

/**
 * A period whose bytes name the sample position they belong to.
 *
 * Every frame and every channel of the period carries its own position, low three
 * bytes first — the width of one L24 sample, so nothing has to be added to the
 * audio for the test to read it back. A period that is stored or played at the
 * wrong position, or whose channels were reordered, therefore fails rather than
 * passing as "some audio arrived".
 */
inline std::vector<uint8_t> make_period(const AudioFormat& format,
                                        uint64_t sample_position) {
  std::vector<uint8_t> period(format.period_bytes(), 0);
  for (size_t frame = 0; frame < format.period_frames; ++frame) {
    const uint64_t position = (sample_position + frame) & k_tag_mask;
    for (size_t channel = 0; channel < format.channels; ++channel) {
      const size_t at = (frame * format.channels + channel) * format.sample_bytes;
      period[at] = static_cast<uint8_t>(position & 0xff);
      period[at + 1] = static_cast<uint8_t>((position >> 8) & 0xff);
      period[at + 2] = static_cast<uint8_t>((position >> 16) & 0xff);
    }
  }
  return period;
}

/** The position the first frame of |period| says it belongs to. */
inline uint64_t position_of(const uint8_t* period) {
  return static_cast<uint64_t>(period[0]) |
         (static_cast<uint64_t>(period[1]) << 8) |
         (static_cast<uint64_t>(period[2]) << 16);
}

/** Fail with the detail when there is any, which is what makes a long run's
 *  failure readable rather than a count with nothing behind it. */
inline void check_clean(const std::string& what, uint64_t count,
                        const std::string& detail = std::string()) {
  if (count == 0) {
    return;
  }
  test::report_failure(what, __FILE__, __LINE__,
                       std::to_string(count) + " time(s) " + detail);
}

/**
 * Arrival jitter, deterministic and repeatable.
 *
 * A link does not deliver on a metronome: SRT schedules a frame's arrival with
 * its TSBPD, so the spread is small, but the spread is exactly what
 * `docs/research/clock-recovery.md` says the direct-estimate alternative depends
 * on and nobody has measured. A loop closed on the level is *supposed* to be
 * immune, because it never measures time — and a claim like that is worth
 * something only if a test puts jitter in front of it.
 *
 * A linear congruential generator rather than `<random>`: the sequence has to be
 * the same on every platform and every run, or "no sample was lost" becomes a
 * statement about the weather.
 */
class Jitter {
 public:
  explicit Jitter(uint64_t seed = 20260917) : state_(seed) {}

  /** A delay in milliseconds, in [0, |spread_ms|]. */
  uint64_t next(uint64_t spread_ms) {
    if (spread_ms == 0) {
      return 0;
    }
    // Numerical Recipes' constants: good enough for a deadline, and portable.
    state_ = state_ * 6364136223846793005ull + 1442695040888963407ull;
    return (state_ >> 33) % (spread_ms + 1);
  }

 private:
  uint64_t state_;
};

}  // namespace clock_sim