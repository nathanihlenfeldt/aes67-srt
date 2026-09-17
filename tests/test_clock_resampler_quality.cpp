#include "clock/resampler.hpp"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "audio/backend.hpp"
#include "clock/playout_buffer.hpp"
#include "clock_simulation.hpp"
#include "test_framework.hpp"

/**
 * What the three converters cost and what they buy, which ADR 0003 deliberately
 * left to the implementation: "a quality-versus-CPU measurement of the available
 * converters belongs to the clock module's implementation, not to this decision".
 *
 * The quality question is not SNR — the documentation gives all three sinc
 * converters 97 dB — it is **bandwidth**: 97%, 90% and 80% of Nyquist, which at
 * 48 kHz means a passband that reaches 23.3 kHz, one that reaches 21.6 kHz, and one
 * that stops at 19.2 kHz and takes the top of the audible band with it. That is
 * measurable on this project's own signal path, so it is measured here rather than
 * quoted.
 *
 * The CPU figure is reported rather than asserted: `docs/research/opus.md` records
 * the same probe reading 39% and 14% for identical work from load alone, so a
 * number taken once inside a test suite is not evidence. The authoritative figure
 * is the Pi's (ticket 18 measured 11.27% of one core at `SINC_FASTEST` for 64
 * channels); what this measures is the *ratio* between the converters, which is
 * what turns that figure into a choice.
 */
namespace {

using aes67_srt::audio::AudioFormat;
using aes67_srt::clock::PlayoutBuffer;
using aes67_srt::clock::PushStatus;
using aes67_srt::clock::Resampler;
using clock_sim::sim_format;

/** A sine at |hz| over the sender's frames. */
float sine(uint64_t frame, double hz) {
  return static_cast<float>(0.25 * std::sin(2.0 * 3.14159265358979 * hz *
                                            static_cast<double>(frame) / 48000.0));
}

/** Fill |buffer| with |push_count| periods of that sine, interleaved. */
void push_sine(PlayoutBuffer* buffer, const AudioFormat& format, size_t push_count,
               double hz) {
  std::vector<uint8_t> period(format.period_bytes(), 0);
  for (size_t push = 0; push < push_count; ++push) {
    for (size_t frame = 0; frame < format.period_frames; ++frame) {
      for (size_t channel = 0; channel < format.channels; ++channel) {
        const int32_t scaled = static_cast<int32_t>(std::lrint(
            static_cast<double>(sine(push * format.period_frames + frame, hz)) *
            8388608.0));
        uint8_t* at = period.data() + (frame * format.channels + channel) * 3;
        at[0] = static_cast<uint8_t>(scaled & 0xff);
        at[1] = static_cast<uint8_t>((scaled >> 8) & 0xff);
        at[2] = static_cast<uint8_t>((scaled >> 16) & 0xff);
      }
    }
    buffer->push(push * format.period_frames, period.data(), period.size());
  }
}

/** The 24-bit value of one sample of a period of bytes. */
int32_t s24_at(const uint8_t* bytes, size_t sample) {
  const uint8_t* at = bytes + sample * 3;
  int32_t value = static_cast<int32_t>(at[0]) | (static_cast<int32_t>(at[1]) << 8) |
                  (static_cast<int32_t>(at[2]) << 16);
  if ((value & 0x800000) != 0) {
    value |= ~0xFFFFFF;
  }
  return value;
}

}  // namespace

/**
 * The measured gain at |hz| after conversion at 1 + 10 ppm.
 *
 * Correlated against the same sine rather than peak-picked: output frame m is the
 * sender's audio from position `m * ratio`, so the reference is the sine at that
 * position and the correlation gives the amplitude of that component and nothing
 * else. A peak pick would be set by one sample and by whatever the filter did to
 * it.
 */
double gain_at(const Resampler::Converter converter, double hz) {
  const size_t channels = 4;
  const unsigned period_frames = 48;
  const size_t pushes = 400;
  const size_t pulls = 380;
  const double ratio = 1.00001;

  AudioFormat format;
  format.sample_rate = 48000;
  format.channels = static_cast<unsigned>(channels);
  format.period_frames = period_frames;
  format.sample_bytes = 3;
  PlayoutBuffer buffer(pushes + 8, format);
  push_sine(&buffer, format, pushes, hz);

  Resampler resampler;
  Resampler::Config config;
  config.channels = static_cast<unsigned>(channels);
  config.period_frames = period_frames;
  config.converter = converter;
  std::string error;
  if (!resampler.open(config, &error)) {
    return 0.0;
  }
  resampler.set_ratio(ratio);

  std::vector<uint8_t> out(format.period_bytes(), 0);
  double sum_sine = 0.0;
  double sum_cosine = 0.0;
  uint64_t counted = 0;
  for (size_t pull = 0; pull < pulls; ++pull) {
    if (!resampler.pull(&buffer, out.data(), period_frames, nullptr, &error)) {
      break;
    }
    if (pull < 40) {
      continue;  // the converter's working room, not a claim about the passband
    }
    for (size_t frame = 0; frame < period_frames; ++frame) {
      const double position =
          static_cast<double>(pull * period_frames + frame) * ratio;
      const double phase = 2.0 * 3.14159265358979 * hz * position / 48000.0;
      const double sample =
          static_cast<double>(s24_at(out.data(), frame * channels)) / 8388608.0;
      sum_sine += sample * std::sin(phase);
      sum_cosine += sample * std::cos(phase);
      ++counted;
    }
  }
  if (counted == 0) {
    return 0.0;
  }
  const double in_phase = sum_sine / static_cast<double>(counted);
  const double quadrature = sum_cosine / static_cast<double>(counted);
  // Correlating a 0.25-amplitude sine against itself gives half its amplitude, so
  // this is the gain relative to the input.
  return 2.0 * std::sqrt(in_phase * in_phase + quadrature * quadrature) / 0.25;
}

/** Seconds one converter takes to convert |seconds| of 8-channel audio. */
double cost_per_second(const Resampler::Converter converter, double seconds,
                       int repeats) {
  const size_t channels = 8;
  const unsigned period_frames = 48;
  AudioFormat format;
  format.sample_rate = 48000;
  format.channels = static_cast<unsigned>(channels);
  format.period_frames = period_frames;
  format.sample_bytes = 3;
  const size_t periods = static_cast<size_t>(seconds * 1000.0);

  double best = 0.0;
  for (int repeat = 0; repeat < repeats; ++repeat) {
    PlayoutBuffer buffer(periods + 8, format);
    push_sine(&buffer, format, periods, 1000.0);
    Resampler resampler;
    Resampler::Config config;
    config.channels = static_cast<unsigned>(channels);
    config.period_frames = period_frames;
    config.converter = converter;
    std::string error;
    if (!resampler.open(config, &error)) {
      return 0.0;
    }
    resampler.set_ratio(1.00001);
    std::vector<uint8_t> out(format.period_bytes(), 0);
    const auto started = std::chrono::steady_clock::now();
    for (size_t pull = 0; pull < periods; ++pull) {
      if (!resampler.pull(&buffer, out.data(), period_frames, nullptr, &error)) {
        break;
      }
    }
    const std::chrono::duration<double> elapsed =
        std::chrono::steady_clock::now() - started;
    if (repeat == 0 || elapsed.count() < best) {
      best = elapsed.count();  // the best of |repeats|: the first is always worst
    }
  }
  return best;
}

TEST_CASE(clock_the_sinc_converters_differ_in_bandwidth_not_in_noise) {
  if (!Resampler::available()) {
    std::cout << "    no libsamplerate in this build: skipping the converter "
                 "measurement"
              << std::endl;
    return;
  }

  // What the CPU buys. The documentation says all three sinc converters are 97 dB
  // and differ in bandwidth; if that were wrong the choice between them would be a
  // different question, so it is checked on this project's own path.
  const double frequencies[] = {1000.0,  10000.0, 19000.0,
                                20000.0, 21000.0, 22000.0};
  const Resampler::Converter converters[] = {Resampler::Converter::sinc_fastest,
                                             Resampler::Converter::sinc_medium,
                                             Resampler::Converter::sinc_best};
  double gain[3][6] = {};
  for (size_t which = 0; which < 3; ++which) {
    for (size_t index = 0; index < 6; ++index) {
      gain[which][index] = gain_at(converters[which], frequencies[index]);
    }
    std::cout << "    resampler quality: "
              << aes67_srt::clock::to_string(converters[which]) << " — 1 kHz "
              << gain[which][0] << ", 19 kHz " << gain[which][2] << ", 21 kHz "
              << gain[which][4] << ", 22 kHz " << gain[which][5] << std::endl;
  }

  // In the passband they are the same converter to within a fraction of a decibel.
  for (size_t which = 0; which < 3; ++which) {
    CHECK_NEAR(gain[which][0], 1.0, 0.05);
    CHECK_NEAR(gain[which][1], 1.0, 0.05);
  }
  // And at the top of the band they are not: the fastest rolls off first and the
  // best is still passing nearly all of it at 22 kHz — the difference an operator
  // would hear on a wideband source, and cannot hear from a specification.
  CHECK(gain[0][5] < gain[1][5]);
  CHECK(gain[1][5] < gain[2][5]);
  CHECK(gain[0][4] < gain[1][4]);
  CHECK(gain[0][5] < 0.9);
  CHECK(gain[2][5] > 0.95);

  // The cost, reported rather than asserted: the ratio between converters is what
  // turns the Pi's measured 11.27% at `SINC_FASTEST` into a choice for the module.
  const double audio_seconds = 0.5;
  const double fastest =
      cost_per_second(Resampler::Converter::sinc_fastest, audio_seconds, 2);
  const double medium =
      cost_per_second(Resampler::Converter::sinc_medium, audio_seconds, 2);
  const double best =
      cost_per_second(Resampler::Converter::sinc_best, audio_seconds, 2);
  std::cout
      << "    resampler cost: 8 channels, seconds per second of audio — fastest "
      << fastest << ", medium " << medium << ", best " << best << std::endl;
  CHECK(fastest > 0.0);
  CHECK(medium > 0.0);
  CHECK(best > 0.0);
}