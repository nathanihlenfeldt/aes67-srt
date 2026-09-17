// The A/V delay line (ticket 12, issue #13).
//
// The load-bearing test is `delay_a_change_mid_stream_is_crossfaded_not_stepped`:
// an operator dial must not click. Audibility itself is a listening test, so what
// is asserted here is the measurable proxy — the largest sample-to-sample step the
// adjustment introduces — against a hard jump, which produces a step of up to
// twice full scale.

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

#include "audio_bytes.hpp"
#include "delay/delay_line.hpp"
#include "test_framework.hpp"

namespace {

using aes67_srt::delay::DelayLine;

constexpr unsigned k_channels = 8;
constexpr unsigned k_period_frames = 48;
constexpr unsigned k_rate = 48000;
constexpr unsigned k_sample_bytes = 3;
constexpr unsigned k_period_bytes = k_channels * k_period_frames * k_sample_bytes;

DelayLine::Config make_config(double capacity_ms, double fade_ms) {
  DelayLine::Config config;
  config.channels = k_channels;
  config.sample_rate = k_rate;
  config.period_frames = k_period_frames;
  config.sample_bytes = k_sample_bytes;
  config.capacity_ms = capacity_ms;
  config.fade_ms = fade_ms;
  return config;
}

/** One period whose every sample carries |value|, so a shift is visible. */
void constant_input(uint64_t value, std::vector<uint8_t>* out) {
  out->assign(k_period_bytes, 0);
  const int32_t sample_value = static_cast<int32_t>(value);
  for (size_t sample = 0; sample < k_period_frames * k_channels; ++sample) {
    audio_bytes::put_s24(out->data() + sample * k_sample_bytes, sample_value);
  }
}

/** One period of a full-scale 1 kHz sine, continuous across periods. */
void sine_input(uint64_t first_frame, std::vector<uint8_t>* out) {
  out->assign(k_period_bytes, 0);
  for (unsigned frame = 0; frame < k_period_frames; ++frame) {
    const double phase = 2.0 * 3.14159265358979323846 * 1000.0 *
                         static_cast<double>(first_frame + frame) / k_rate;
    const int32_t value =
        static_cast<int32_t>(std::lround(0.9 * 8388607.0 * std::sin(phase)));
    for (unsigned channel = 0; channel < k_channels; ++channel) {
      audio_bytes::put_s24(
          out->data() + (frame * k_channels + channel) * k_sample_bytes, value);
    }
  }
}

int32_t sample(const std::vector<uint8_t>& period, unsigned frame) {
  return audio_bytes::s24_at(period.data(), frame * k_channels);
}

/** The largest step channel 0 takes from |previous| through |period|. */
int32_t max_step(const std::vector<uint8_t>& period, int32_t* previous) {
  int32_t largest = 0;
  for (unsigned frame = 0; frame < k_period_frames; ++frame) {
    const int32_t value = sample(period, frame);
    const int32_t step = std::abs(value - *previous);
    if (step > largest) {
      largest = step;
    }
    *previous = value;
  }
  return largest;
}

}  // namespace

TEST_CASE(delay_zero_offset_is_a_memcpy) {
  DelayLine line;
  std::string error;
  CHECK(line.open(make_config(1000.0, 10.0), &error));
  CHECK(line.set_offset_ms(0.0, &error));

  std::vector<uint8_t> input;
  std::vector<uint8_t> output(k_period_bytes, 0);
  for (uint64_t period = 0; period < 20; ++period) {
    constant_input(period, &input);
    CHECK(line.process(input.data(), output.data(), k_period_frames, &error));
    CHECK(output == input);
  }
  CHECK_EQ(line.crossfades(), 0u);
}

TEST_CASE(delay_holds_audio_for_the_offset_it_was_asked_for) {
  DelayLine line;
  std::string error;
  CHECK(line.open(make_config(1000.0, 10.0), &error));
  // 10 ms is ten periods at 48 frames each, so the shift is a whole number of
  // periods and each period comes back untouched.
  CHECK(line.set_offset_ms(10.0, &error));
  CHECK_NEAR(line.applied_offset_ms(), 10.0, 1e-9);

  std::vector<uint8_t> input;
  std::vector<uint8_t> output(k_period_bytes, 0);
  for (uint64_t period = 0; period < 30; ++period) {
    constant_input(period + 1, &input);
    CHECK(line.process(input.data(), output.data(), k_period_frames, &error));
    if (period < 10) {
      CHECK(sample(output, 0) == 0);  // nothing has been heard yet
    } else {
      CHECK_EQ(sample(output, 0), static_cast<int32_t>(period - 10 + 1));
    }
  }
  CHECK_EQ(line.crossfades(), 0u);
}

TEST_CASE(delay_a_change_mid_stream_is_crossfaded_not_stepped) {
  DelayLine line;
  std::string error;
  CHECK(line.open(make_config(2000.0, 10.0), &error));
  CHECK(line.set_offset_ms(0.0, &error));

  std::vector<uint8_t> input;
  std::vector<uint8_t> output(k_period_bytes, 0);
  uint64_t frame = 0;
  int32_t previous = 0;

  // A second of programme at zero offset first, so the line is full.
  int32_t programme_step = 0;
  for (unsigned period = 0; period < 1000; ++period) {
    sine_input(frame, &input);
    CHECK(line.process(input.data(), output.data(), k_period_frames, &error));
    const int32_t step = max_step(output, &previous);
    if (period >= 900 && step > programme_step) {
      programme_step = step;  // what the audio steps by with no adjustment
    }
    frame += k_period_frames;
  }
  CHECK(programme_step > 0);

  // A change is allowed to blend, so it may add the weighted difference of two
  // samples: at most 2 x full scale across the fade, which for a 10 ms window is
  // well under 1/8 of full scale. It must not add a *discontinuity*, so the step
  // may not exceed what the programme steps by plus that blend bound. A hard jump
  // steps by up to twice full scale — ~60x the bound — which is why this can fail.
  const int32_t blend_bound = 8388607 / 8;
  const double offsets[] = {137.3, 20.0};
  for (double offset : offsets) {
    CHECK(line.set_offset_ms(offset, &error));
    int32_t largest = 0;
    for (unsigned period = 0; period < 100; ++period) {
      sine_input(frame, &input);
      CHECK(line.process(input.data(), output.data(), k_period_frames, &error));
      const int32_t step = max_step(output, &previous);
      if (step > largest) {
        largest = step;
      }
      frame += k_period_frames;
    }
    CHECK(largest <= programme_step + blend_bound);
    CHECK(!line.adjusting());
    CHECK_NEAR(line.applied_offset_ms(), offset, 0.02);
  }
  CHECK_EQ(line.crossfades(), 2u);
}

TEST_CASE(delay_a_change_that_arrives_mid_fade_is_not_lost) {
  DelayLine line;
  std::string error;
  CHECK(line.open(make_config(2000.0, 20.0), &error));
  CHECK(line.set_offset_ms(0.0, &error));

  std::vector<uint8_t> input;
  std::vector<uint8_t> output(k_period_bytes, 0);
  uint64_t frame = 0;
  for (unsigned period = 0; period < 200; ++period) {
    sine_input(frame, &input);
    CHECK(line.process(input.data(), output.data(), k_period_frames, &error));
    frame += k_period_frames;
  }

  // Two requests inside one fade window: the final value is the one that lands.
  CHECK(line.set_offset_ms(1000.0, &error));
  sine_input(frame, &input);
  CHECK(line.process(input.data(), output.data(), k_period_frames, &error));
  frame += k_period_frames;
  CHECK(line.adjusting());
  CHECK(line.set_offset_ms(50.0, &error));
  for (unsigned period = 0; period < 2000; ++period) {
    sine_input(frame, &input);
    CHECK(line.process(input.data(), output.data(), k_period_frames, &error));
    frame += k_period_frames;
  }
  CHECK(!line.adjusting());
  CHECK_NEAR(line.applied_offset_ms(), 50.0, 0.02);
}

TEST_CASE(delay_refuses_an_offset_past_its_capacity) {
  DelayLine line;
  std::string error;
  CHECK(line.open(make_config(100.0, 10.0), &error));
  CHECK(!line.set_offset_ms(100.1, &error));
  CHECK(!error.empty());
  CHECK(!line.set_offset_ms(-0.1, &error));
  CHECK(line.set_offset_ms(100.0, &error));
  CHECK_NEAR(line.offset_ms(), 100.0, 0.02);
}

TEST_CASE(delay_output_before_the_line_has_filled_is_silence) {
  DelayLine line;
  std::string error;
  CHECK(line.open(make_config(1000.0, 10.0), &error));
  CHECK(line.set_offset_ms(50.0, &error));  // 50 ms = 50 periods

  std::vector<uint8_t> input;
  std::vector<uint8_t> output(k_period_bytes, 0);
  for (uint64_t period = 0; period < 50; ++period) {
    constant_input(period + 1, &input);
    CHECK(line.process(input.data(), output.data(), k_period_frames, &error));
    CHECK(sample(output, 0) == 0);  // 50 ms asked for, 50 ms must fill first
  }
  constant_input(51, &input);
  CHECK(line.process(input.data(), output.data(), k_period_frames, &error));
  CHECK_EQ(sample(output, 0), 1);  // the first period it heard, 50 ms later
}

TEST_CASE(delay_refuses_a_geometry_it_cannot_build) {
  DelayLine line;
  std::string error;
  DelayLine::Config config = make_config(100.0, 10.0);
  config.channels = 0;
  CHECK(!line.open(config, &error));
  CHECK(!error.empty());
  config = make_config(100.0, 10.0);
  config.sample_bytes = 4;
  CHECK(!line.open(config, &error));
  CHECK(!line.is_open());
}
