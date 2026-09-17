#include "clock/resampler.hpp"

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
 * The resampler (ticket 11, issue #12; increment 3 of the clock module).
 *
 * ADR 0003 decided continuous conversion; the ratio control decides how fast the
 * sender's audio must be consumed; this is what consumes it. The tests here are
 * about the three things that can go wrong silently: the *direction* of the ratio
 * (libsamplerate's `src_ratio` is output rate over input rate, the reciprocal of
 * what the control produces), the *carry* (input the converter did not consume has
 * to be kept, or every later period is displaced by a frame), and the *bytes*
 * (24-bit audio through a float path, lossless if it is done right and a quiet
 * disaster if it is not).
 *
 * The converter's own quality — bandwidth and CPU, the choice ADR 0003 leaves to
 * the implementation — is measured in `test_clock_resampler_quality.cpp`, so that
 * the numbers that *decide* something stay separate from the numbers that *prove*
 * something.
 */
namespace {

using aes67_srt::audio::AudioFormat;
using aes67_srt::clock::float_to_s24_3le;
using aes67_srt::clock::parse_converter;
using aes67_srt::clock::PlayoutBuffer;
using aes67_srt::clock::PushStatus;
using aes67_srt::clock::Resampler;
using aes67_srt::clock::s24_3le_to_float;
using clock_sim::sim_format;

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

/** Put one 24-bit value into a byte triple. */
void put_s24(uint8_t* at, int32_t value) {
  at[0] = static_cast<uint8_t>(value & 0xff);
  at[1] = static_cast<uint8_t>((value >> 8) & 0xff);
  at[2] = static_cast<uint8_t>((value >> 16) & 0xff);
}

/** A period whose every sample is |value|, so a wrong sample is obvious. */
std::vector<uint8_t> constant_period(const AudioFormat& format, int32_t value) {
  std::vector<uint8_t> period(format.period_bytes(), 0);
  for (size_t sample = 0; sample < format.period_frames * format.channels;
       ++sample) {
    put_s24(period.data() + sample * 3, value);
  }
  return period;
}

bool contains(const std::string& text, const std::string& needle) {
  return text.find(needle) != std::string::npos;
}

}  // namespace

TEST_CASE(clock_bytes_become_floats_and_come_back_unchanged) {
  // The one place in this project where a sample is converted, and ADR 0003's own
  // requirement is that it be lossless: every 24-bit integer is representable in a
  // float, so the conversion costs nothing and the converter's filter is the only
  // thing that touches the audio.
  const int32_t values[] = {0,        1,       -1,       2,       8388607,
                            -8388608, 4194304, -4194304, 1234567, -7654321};
  for (const int32_t value : values) {
    uint8_t bytes[3];
    put_s24(bytes, value);
    float sample = 0.0f;
    s24_3le_to_float(bytes, 1, 1, &sample);
    // In [-1, 1), and exactly what a signed 24-bit sample should be.
    CHECK_NEAR(static_cast<double>(sample), static_cast<double>(value) / 8388608.0,
               1e-12);
    CHECK(sample >= -1.0f);
    CHECK(sample < 1.0f);

    uint8_t back[3] = {0, 0, 0};
    float_to_s24_3le(&sample, 1, 1, back);
    CHECK_EQ(s24_at(back, 0), value);
  }

  // And through a whole interleaved period, in order: the conversion walks the
  // buffer the same way the frame does.
  const AudioFormat format = sim_format(2);
  std::vector<uint8_t> bytes(format.period_bytes(), 0);
  for (size_t sample = 0; sample < format.period_frames * format.channels;
       ++sample) {
    put_s24(bytes.data() + sample * 3,
            static_cast<int32_t>(sample) * 7919 - 4000000);
  }
  std::vector<float> floats(format.period_frames * format.channels, 0.0f);
  s24_3le_to_float(bytes.data(), format.period_frames, format.channels,
                   floats.data());
  std::vector<uint8_t> back(format.period_bytes(), 0);
  float_to_s24_3le(floats.data(), format.period_frames, format.channels,
                   back.data());
  CHECK(back == bytes);
}

TEST_CASE(clock_a_sample_that_overflows_the_ceiling_is_clipped_not_wrapped) {
  // A converter can overshoot — that is what a filter does to a signal at full
  // scale — and an overshoot that wrapped would be a click the size of the signal
  // instead of a sample at the ceiling.
  const float too_loud[] = {1.5f, -1.5f, 2.0f, -2.0f, 1.0000001f};
  const size_t count = sizeof(too_loud) / sizeof(too_loud[0]);
  std::vector<uint8_t> bytes(count * 3, 0);
  float_to_s24_3le(too_loud, count, 1, bytes.data());
  CHECK_EQ(s24_at(bytes.data(), 0), 8388607);
  CHECK_EQ(s24_at(bytes.data(), 1), -8388608);
  CHECK_EQ(s24_at(bytes.data(), 2), 8388607);
  CHECK_EQ(s24_at(bytes.data(), 3), -8388608);
  CHECK_EQ(s24_at(bytes.data(), 4), 8388607);

  // A NaN is not a loud sample: it is silence, because the ceiling is the loudest
  // thing that may come out of here and a NaN is not audio at all.
  const float broken[] = {std::nanf(""), 0.5f};
  std::vector<uint8_t> out(6, 0xff);
  float_to_s24_3le(broken, 2, 1, out.data());
  CHECK_EQ(s24_at(out.data(), 0), 0);
  CHECK_EQ(s24_at(out.data(), 1), 4194304);
}

TEST_CASE(clock_the_converter_names_round_trip_and_an_unknown_one_is_refused) {
  Resampler::Converter converter = Resampler::Converter::sinc_best;
  for (const auto candidate :
       {Resampler::Converter::sinc_best, Resampler::Converter::sinc_medium,
        Resampler::Converter::sinc_fastest}) {
    CHECK(parse_converter(aes67_srt::clock::to_string(candidate), &converter));
    CHECK(converter == candidate);
  }
  CHECK(!parse_converter("sinc_enormous", &converter));
  CHECK(!parse_converter("", &converter));
  // A refusal leaves what it was given rather than half-parsing it.
  CHECK(converter == Resampler::Converter::sinc_fastest);
}

namespace {

/**
 * What a run through the resampler produced.
 *
 * The input is kept as floats as well as bytes, because the two claims worth
 * testing are both comparisons between what went in and what came out: that the
 * *rate* came out right (the ratio's direction), and that the signal did (the
 * carry, and the converter's own delay).
 */
struct Feed {
  std::vector<float> input;   // frames x channels, in the order pushed
  std::vector<float> output;  // frames x channels, in the order pulled
  size_t channels = 0;
  size_t pushes = 0;
  size_t pulls = 0;
  uint64_t first_position = 0;
  uint64_t last_position = 0;
  bool opened = false;
  std::string error;
  bool short_pull = false;
  /** The resampler's own counters, copied after the run. */
  uint64_t input_frames = 0;
  uint64_t output_frames = 0;
  uint64_t short_pulls = 0;
  uint64_t held_frames = 0;
  uint64_t buffer_held = 0;
  uint64_t buffer_played = 0;
  uint64_t pending_frames = 0;
};
/** A signal, as a function of the sender's frame index and channel. */
using Signal = std::function<float(uint64_t frame, unsigned channel)>;

/**
 * Push |pushes| periods into a buffer and pull |pulls| periods back through the
 * resampler.
 *
 * The buffer is filled before anything is pulled, which is not how the engine
 * works but is what makes each test's arithmetic legible: every input frame is
 * known, so the output can be compared with it frame by frame.
 */
Feed run_resampler(size_t channels, unsigned period_frames,
                   Resampler::Converter converter, double ratio, size_t pushes,
                   size_t pulls, const Signal& signal, bool carried_trace = false) {
  Feed feed;
  feed.channels = channels;
  feed.pushes = pushes;
  feed.pulls = pulls;

  AudioFormat format;
  format.sample_rate = 48000;
  format.channels = static_cast<unsigned>(channels);
  format.period_frames = period_frames;
  format.sample_bytes = 3;

  PlayoutBuffer buffer(pushes + 16, format);
  Resampler resampler;
  Resampler::Config config;
  config.channels = static_cast<unsigned>(channels);
  config.period_frames = period_frames;
  config.converter = converter;
  if (!resampler.open(config, &feed.error)) {
    return feed;
  }
  feed.opened = true;
  resampler.set_ratio(ratio);

  std::vector<uint8_t> period(format.period_bytes(), 0);
  for (size_t push = 0; push < pushes; ++push) {
    const uint64_t position = push * period_frames;
    for (size_t frame = 0; frame < period_frames; ++frame) {
      for (size_t channel = 0; channel < channels; ++channel) {
        const float value =
            signal(position + frame, static_cast<unsigned>(channel));
        const int32_t scaled = static_cast<int32_t>(
            std::lrint(static_cast<double>(value) * 8388608.0));
        put_s24(period.data() + (frame * channels + channel) * 3, scaled);
        feed.input.push_back(static_cast<float>(scaled) / 8388608.0f);
      }
    }
    if (buffer.push(position, period.data(), period.size()) != PushStatus::stored) {
      feed.error = "the buffer refused a period";
      return feed;
    }
  }

  std::vector<uint8_t> out(format.period_bytes(), 0);
  for (size_t pull = 0; pull < pulls; ++pull) {
    uint64_t position = 0;
    if (!resampler.pull(&buffer, out.data(), period_frames, &position,
                        &feed.error)) {
      feed.short_pull = true;
      break;
    }
    if (carried_trace && (pull < 4 || pull + 2 >= pulls)) {
      std::cout << "      pull " << pull << ": consumed "
                << resampler.input_frames() << ", produced "
                << resampler.output_frames() << ", held " << resampler.held_frames()
                << std::endl;
    }
    if (pull == 0) {
      feed.first_position = position;
    }
    feed.last_position = position;
    for (size_t sample = 0; sample < period_frames * channels; ++sample) {
      feed.output.push_back(static_cast<float>(s24_at(out.data(), sample)) /
                            8388608.0f);
    }
  }
  feed.input_frames = resampler.input_frames();
  feed.output_frames = resampler.output_frames();
  feed.short_pulls = resampler.short_pulls();
  feed.held_frames = resampler.held_frames();
  feed.buffer_held = buffer.held_frames();
  feed.buffer_played = buffer.frames_played();
  feed.pending_frames = resampler.pending_frames();
  return feed;
}

/** A sine, quiet enough that the converter's overshoot cannot clip: the peak of
 *  every channel differs, so a channel that moved would be caught. */
float tone(uint64_t frame, unsigned channel) {
  const double amplitude = 0.2 + 0.05 * channel;
  const double phase = 0.13 * channel;
  return static_cast<float>(amplitude *
                            std::sin(2.0 * 3.14159265358979 * 1007.0 *
                                         static_cast<double>(frame) / 48000.0 +
                                     phase));
}

}  // namespace

TEST_CASE(clock_the_ratio_consumes_the_input_at_the_rate_the_loop_asked_for) {
  if (!Resampler::available()) {
    std::cout << "    no libsamplerate in this build: skipping the resampler"
              << std::endl;
    return;
  }

  // The direction, which is the one thing here that fails quietly. libsamplerate's
  // `src_ratio` is *output* rate over *input* rate; the control produces input
  // frames consumed per output frame. So a control ratio above one — the sender's
  // clock is the faster, the buffer is filling — has to consume *more* input per
  // output period, and a reciprocal taken the wrong way would do the opposite and
  // drive the level to the buffer's floor.
  //
  // First at ratios far enough apart that nothing can hide, which is also what
  // exercises the carry hardest: at 0.9 the converter consumes four and a half
  // frames fewer than it produces every period, and at 1.1 four and a half more,
  // so the carry has to grow and shrink to match.
  const size_t pulls = 2000;
  const size_t pushes = 3000;
  const Feed wider = run_resampler(4, 48, Resampler::Converter::sinc_medium, 1.1,
                                   pushes, pulls, tone, /*carried_trace=*/true);
  const Feed narrower = run_resampler(4, 48, Resampler::Converter::sinc_medium, 0.9,
                                      pushes, pulls, tone);
  CHECK(wider.opened);
  CHECK(narrower.opened);
  CHECK_EQ(wider.output_frames, pulls * 48u);
  CHECK_EQ(narrower.output_frames, pulls * 48u);
  // Whole periods rather than whole frames: the library consumes 48 input frames or
  // 96 per pull and the ratio lives in the average, so a couple of frames of
  // slack is not the tolerance — a whole period either way is.
  CHECK_NEAR(static_cast<double>(wider.input_frames), pulls * 48.0 * 1.1, 96.0);
  CHECK_NEAR(static_cast<double>(narrower.input_frames), pulls * 48.0 * 0.9, 96.0);

  // And then at the scale the loop actually works on, where one thing about the
  // library has to be understood first: **what it consumes is quantised to whole
  // periods.** Measured per pull, it takes either 48 input frames or 96, and the
  // ratio lives in the *average*. So a single pull cannot show ten ppm — a 10 ppm
  // difference is one frame per 4800 periods — and what can be tested is the
  // effective rate over a long run: input over output, which has to be the ratio.
  //
  // One channel and a long run, because the test's cost is the audio it converts:
  // 200,000 periods is 9.6 million frames, and the quantisation's residue (one
  // period) is then 5 parts per million, so a 10 ppm ratio is a 2:1 measurement.
  const size_t long_pulls = 200000;
  const size_t long_pushes = long_pulls + 16;
  const Feed long_run = run_resampler(1, 48, Resampler::Converter::sinc_medium,
                                      1.00001, long_pushes, long_pulls, tone);
  CHECK(long_run.opened);
  CHECK_EQ(long_run.output_frames, long_pulls * 48u);
  const uint64_t held = long_run.held_frames;
  const double steady = static_cast<double>(long_run.input_frames - held) /
                        static_cast<double>(long_run.output_frames);
  const double steady_ppm = (steady - 1.0) * 1e6;
  // The raw rate reads high by exactly the audio the library is holding — one
  // period over 9.6 million frames is 5 ppm — which is why the steady figure takes
  // the hold out first. What is being verified is the *rate*, and the hold is a
  // constant the run carries.
  CHECK_NEAR(static_cast<double>(long_run.input_frames) /
                     static_cast<double>(long_run.output_frames) * 1e6 -
                 1e6,
             steady_ppm + 1e6 * static_cast<double>(held) /
                              static_cast<double>(long_run.output_frames),
             1e-6);
  CHECK_NEAR(steady_ppm, 10.0, 2.0);

  std::cout << "    resampler: ratio 1.1/0.9 consumed " << wider.input_frames << "/"
            << narrower.input_frames << " frames for " << pulls * 48
            << " out; over " << long_pulls << " periods the steady "
            << "ratio was " << steady_ppm << " ppm (asked for 10)" << std::endl;

  // What the library holds, which is the number the A/V figure needs: it takes more
  // input than it produces and keeps the difference. Measured as one to two periods
  // depending on the ratio, and bounded here so that a library which changed its
  // working buffer would fail rather than move the alignment.
  CHECK(held > 0u);
  CHECK(held <= 4 * 48u);
  std::cout << "    resampler: the converter holds " << held << " frames ("
            << (held / 48.0) << " ms) of input it has not produced" << std::endl;
}

TEST_CASE(clock_the_converter_delays_the_audio_by_the_table_it_claims) {
  if (!Resampler::available()) {
    std::cout << "    no libsamplerate in this build: skipping the resampler delay"
              << std::endl;
    return;
  }

  // ADR 0003 requires the A/V delay line to know the resampler's delay alongside
  // the codec's, and the library documents no figure for it. So it is measured
  // here, on a tone, by finding the shift that best lines the output up with the
  // input — and the table the class reports is asserted against that measurement,
  // so a library that changes its filters fails this test instead of moving the
  // alignment.
  for (const auto converter :
       {Resampler::Converter::sinc_fastest, Resampler::Converter::sinc_medium,
        Resampler::Converter::sinc_best}) {
    const Feed feed = run_resampler(1, 48, converter, 1.0, 400, 399, tone);
    CHECK(feed.opened);
    const size_t frames = feed.output.size();
    CHECK(frames > 15000u);

    // The best integer shift, over the settled part of the run: the first frames
    // are the filter filling up, and nothing there is a claim about the converter.
    const size_t skip = 2000;
    size_t best_lag = 0;
    double best_error = 0.0;
    for (size_t lag = 0; lag < 400; ++lag) {
      double sum = 0.0;
      size_t count = 0;
      for (size_t frame = skip; frame + lag < frames; ++frame) {
        const double expected =
            static_cast<double>(feed.input[(frame + lag) * feed.channels]);
        const double difference =
            static_cast<double>(feed.output[frame * feed.channels]) - expected;
        sum += difference * difference;
        ++count;
      }
      const double error = std::sqrt(sum / static_cast<double>(count));
      if (lag == 0 || error < best_error) {
        best_error = error;
        best_lag = lag;
      }
    }

    Resampler resampler;
    Resampler::Config config;
    config.channels = 1;
    config.period_frames = 48;
    config.converter = converter;
    std::string error;
    CHECK(resampler.open(config, &error));
    const unsigned claimed = resampler.delay_frames();

    std::cout << "    resampler: " << aes67_srt::clock::to_string(converter)
              << " delay measured " << best_lag << " frames, table says " << claimed
              << ", residual " << best_error << std::endl;
    CHECK(static_cast<int>(best_lag) - static_cast<int>(claimed) <= 2);
    CHECK(static_cast<int>(claimed) - static_cast<int>(best_lag) <= 2);
    // At ratio 1 a sinc converter is a delayed copy of its input, so what is left
    // after the best shift is the filter's own error on a 1 kHz tone. If the carry
    // dropped or repeated a frame the residual would be a step in phase, and this
    // would be tens of times larger.
    CHECK(best_error < 0.002);
  }
}

TEST_CASE(clock_the_carry_never_loses_or_repeats_a_frame_of_the_signal) {
  if (!Resampler::available()) {
    std::cout << "    no libsamplerate in this build: skipping the carry test"
              << std::endl;
    return;
  }

  // The carry, tested on the *signal* against the position it should be at, because
  // a ratio away from one means the output frame and the input frame do not line up
  // one for one: frame m of the output is the sender's audio from position
  // `m * ratio`, plus the converter's own offset. The signal is a known function,
  // so that position can be evaluated exactly rather than interpolated, and a frame
  // dropped or repeated anywhere in the run shows up as the *offset changing* —
  // which is what this measures, in each half of the run separately.
  const double ratio = 1.00001;
  const Feed feed = run_resampler(2, 48, Resampler::Converter::sinc_medium, ratio,
                                  2000, 1990, tone);
  CHECK(feed.opened);
  const size_t channels = feed.channels;
  const size_t frames = feed.output.size() / channels;
  CHECK(frames > 90000u);

  /** The gate that best lines the output up with the sender's position over
   *  [from, to), and the residual it leaves. */
  const auto fit = [&](size_t from, size_t to, double* offset, double* residual) {
    double best_offset = 0.0;
    double best_error = 0.0;
    for (int step = -200; step <= 200; ++step) {
      const double gate = static_cast<double>(step) / 100.0;
      double sum = 0.0;
      size_t count = 0;
      for (size_t frame = from; frame < to; ++frame) {
        const double position = static_cast<double>(frame) * ratio + gate;
        for (size_t channel = 0; channel < channels; ++channel) {
          const double expected =
              static_cast<double>(tone(static_cast<uint64_t>(position), channel));
          const double difference =
              static_cast<double>(feed.output[frame * channels + channel]) -
              expected;
          sum += difference * difference;
          ++count;
        }
      }
      const double error = std::sqrt(sum / static_cast<double>(count));
      if (step == -200 || error < best_error) {
        best_error = error;
        best_offset = gate;
      }
    }
    *offset = best_offset;
    *residual = best_error;
  };

  // Skip the first frames: the converter is filling its working room, and nothing
  // there is a claim about the carry.
  const size_t settled = 2000;
  const size_t middle = settled + (frames - settled) / 2;
  double first_offset = 0.0;
  double first_residual = 0.0;
  double second_offset = 0.0;
  double second_residual = 0.0;
  fit(settled, middle, &first_offset, &first_residual);
  fit(middle, frames, &second_offset, &second_residual);

  // And the ledger, which is the exact version of the same claim: everything the
  // buffer handed over is either in the converter or still in the carry, to the
  // frame. A frame lost in the carry would leave this short and would be audio lost
  // with nothing in the log.
  CHECK_EQ(feed.buffer_played, feed.input_frames + feed.pending_frames);

  std::cout << "    resampler: 10 ppm over " << frames
            << " frames — the buffer gave " << feed.buffer_played
            << " frames, the converter took " << feed.input_frames << " and "
            << feed.pending_frames << " are still pending; residuals "
            << first_residual << " and " << second_residual << std::endl;

  // A quarter-scale tone, so this is 20% of it. The floor here is the *model's* own
  // error — evaluating a tone at a fractional position is not what a converter does
  // — and a carry that handed the converter a stale frame would be the whole
  // signal.
  CHECK(first_residual < 0.05);
  CHECK(second_residual < 0.05);
  // The offset between the output and the sender's position is the same across the
  // run to within the model's error: a whole frame dropped or repeated would move
  // it by a frame, which this would see.
  CHECK(std::fabs(second_offset - first_offset) < 1.0);
  CHECK(std::fabs(first_offset) < 2.0);
}

TEST_CASE(clock_an_empty_buffer_is_refused_rather_than_padded_or_invented) {
  if (!Resampler::available()) {
    std::cout << "    no libsamplerate in this build: skipping the underrun test"
              << std::endl;
    return;
  }

  const AudioFormat format = sim_format(4);
  PlayoutBuffer buffer(64, format);
  Resampler resampler;
  Resampler::Config config;
  config.channels = 4;
  config.period_frames = 48;
  std::string error;
  CHECK(resampler.open(config, &error));
  resampler.set_ratio(1.0);

  std::vector<uint8_t> out(format.period_bytes(), 0xab);
  // Nothing has arrived, so nothing can be played: the refusal is a state, and the
  // caller's audio has not been touched — it still holds the caller's own marker,
  // not silence this class decided to write.
  CHECK(!resampler.pull(&buffer, out.data(), 48, nullptr, &error));
  for (const uint8_t byte : out) {
    CHECK_EQ(static_cast<int>(byte), 0xab);
  }
  CHECK_EQ(resampler.short_pulls(), 0u);
  CHECK_EQ(resampler.input_frames(), 0u);

  // One period arrives, and one period is not enough for the converter to fill a
  // period: the first frames are audio — the filter's transient response, ramping
  // up from silence — and the rest is silence, counted rather than hidden. A
  // receiver needs its level primed before its first period comes out, which is
  // what the engine's priming has to allow for.
  std::vector<uint8_t> period = constant_period(format, 1000000);
  CHECK(buffer.push(0, period.data(), period.size()) == PushStatus::stored);
  CHECK(resampler.pull(&buffer, out.data(), 48, nullptr, &error));
  CHECK(s24_at(out.data(), 0) > 0);
  CHECK_EQ(resampler.short_pulls(), 1u);
  size_t tail_zeros = 0;
  for (size_t frame = 0; frame < 48; ++frame) {
    if (s24_at(out.data(), frame * 4) == 0) {
      ++tail_zeros;
    }
  }
  CHECK(tail_zeros > 0u);

  // And now the buffer is dry again with nothing carried: the same refusal, because
  // there is genuinely nothing to play.
  for (uint8_t& byte : out) {
    byte = 0xab;
  }
  CHECK(!resampler.pull(&buffer, out.data(), 48, nullptr, &error));
  CHECK_EQ(static_cast<int>(out[0]), 0xab);
}

TEST_CASE(clock_the_resampler_refuses_what_it_cannot_do) {
  // Refusals that name their reason, because the alternative is an appliance that
  // sounds wrong for a reason nothing reports.
  Resampler resampler;
  std::string error;
  std::vector<uint8_t> out(64 * 48 * 3, 0);
  PlayoutBuffer buffer(8, sim_format(4));

  // Not open yet.
  CHECK(!resampler.pull(&buffer, out.data(), 48, nullptr, &error));
  CHECK(contains(error, "not open"));
  CHECK(!resampler.is_open());
  CHECK_EQ(resampler.input_frames(), 0u);
  CHECK_EQ(resampler.pending_frames(), 0u);
  CHECK_EQ(resampler.delay_frames(), 0u);
  CHECK_NEAR(resampler.ratio(), 1.0, 1e-12);
  resampler.close();         // closing what is not open is not an error
  resampler.set_ratio(2.0);  // and neither is setting a ratio on it

  if (!Resampler::available()) {
    std::cout << "    no libsamplerate in this build: skipping the refusal reasons"
              << std::endl;
    CHECK(!resampler.open(Resampler::Config(), &error));
    CHECK(contains(error, "libsamplerate"));
    CHECK(contains(Resampler::unavailable_reason(), "libsamplerate"));
    return;
  }

  // A configuration that is not audio.
  Resampler::Config broken;
  broken.channels = 0;
  CHECK(!resampler.open(broken, &error));
  CHECK(contains(error, "channel count"));

  Resampler::Config config;
  config.channels = 4;
  config.period_frames = 48;
  CHECK(resampler.open(config, &error));
  CHECK(resampler.is_open());

  // A pull bigger than a period, which is a caller that has lost track of what a
  // period is; a pull of nothing; and a pull with no buffer.
  CHECK(!resampler.pull(&buffer, out.data(), 49, nullptr, &error));
  CHECK(contains(error, "one period"));
  CHECK(!resampler.pull(&buffer, out.data(), 0, nullptr, &error));
  CHECK(contains(error, "no audio"));
  CHECK(!resampler.pull(nullptr, out.data(), 48, nullptr, &error));
  CHECK(contains(error, "no audio"));

  // And the ratio: a number that is not a positive clock difference does not move
  // it, and one far outside what a crystal pair can be is bounded rather than
  // passed to the library.
  resampler.set_ratio(1.00001);
  CHECK_NEAR(resampler.ratio(), 1.00001, 1e-12);
  resampler.set_ratio(std::nan(""));
  CHECK_NEAR(resampler.ratio(), 1.00001, 1e-12);
  resampler.set_ratio(0.0);
  CHECK_NEAR(resampler.ratio(), 1.00001, 1e-12);
  resampler.set_ratio(-1.0);
  CHECK_NEAR(resampler.ratio(), 1.00001, 1e-12);
  resampler.set_ratio(1000.0);
  CHECK_NEAR(resampler.ratio(), 2.0, 1e-12);
  resampler.set_ratio(0.001);
  CHECK_NEAR(resampler.ratio(), 0.5, 1e-12);
}

TEST_CASE(clock_a_pull_short_of_input_is_silence_rather_than_a_refusal) {
  if (!Resampler::available()) {
    std::cout << "    no libsamplerate in this build: skipping the short pull test"
              << std::endl;
    return;
  }

  // The case between "the buffer had nothing" and "the buffer had everything": the
  // audio runs out *during* a pull. The device is owed a whole period either way,
  // so what is missing is silence and the pull is counted — a receiver that is fed
  // silence it can see in a number, rather than silence nothing reports.
  //
  // Built with a ratio far outside anything two crystals can be (the clamp's own
  // limit), because that is what makes the converter ask for more input than a
  // single period can supply: at ratio 2 it needs two periods for every one it
  // produces, and here it has one.
  const AudioFormat format = sim_format(4);
  PlayoutBuffer buffer(64, format);
  Resampler resampler;
  Resampler::Config config;
  config.channels = 4;
  config.period_frames = 48;
  std::string error;
  CHECK(resampler.open(config, &error));
  resampler.set_ratio(2.0);
  CHECK_NEAR(resampler.ratio(), 2.0, 1e-12);

  const std::vector<uint8_t> loud = constant_period(format, 1000000);
  CHECK(buffer.push(0, loud.data(), loud.size()) == PushStatus::stored);
  std::vector<uint8_t> out(format.period_bytes(), 0xab);
  CHECK(resampler.pull(&buffer, out.data(), 48, nullptr, &error));
  CHECK_EQ(resampler.short_pulls(), 1u);

  // Fed, and what it was fed is silence — not the caller's marker, and not audio
  // repeated from before to fill the gap.
  size_t zeros = 0;
  for (size_t sample = 0; sample < 48 * 4; ++sample) {
    if (s24_at(out.data(), sample) == 0) {
      ++zeros;
    }
  }
  CHECK_EQ(zeros, static_cast<size_t>(48 * 4));

  std::cout
      << "    resampler: a pull that could not be filled was counted and padded"
      << std::endl;
}