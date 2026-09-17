// The impulse test signal (ticket 12, issue #13): sample-accurate and repeatable.

#include <cstdint>
#include <string>
#include <vector>

#include "audio_bytes.hpp"
#include "delay/test_signal.hpp"
#include "test_framework.hpp"

namespace {

using aes67_srt::delay::TestSignal;

constexpr unsigned k_channels = 8;
constexpr unsigned k_period_frames = 48;
constexpr unsigned k_sample_bytes = 3;
constexpr unsigned k_period_bytes = k_channels * k_period_frames * k_sample_bytes;

TestSignal::Config make_config(int channel) {
  TestSignal::Config config;
  config.channels = k_channels;
  config.sample_rate = 48000;
  config.period_frames = k_period_frames;
  config.sample_bytes = k_sample_bytes;
  config.channel = channel;
  return config;
}

int32_t at(const std::vector<uint8_t>& period, unsigned frame, unsigned channel) {
  return audio_bytes::s24_at(period.data(), frame * k_channels + channel);
}

}  // namespace

TEST_CASE(test_signal_an_impulse_lands_on_exactly_the_frame_it_was_asked_for) {
  TestSignal signal;
  std::string error;
  CHECK(signal.open(make_config(3), &error));
  CHECK(signal.trigger(100, &error));

  std::vector<uint8_t> period(k_period_bytes, 0);
  // A period that starts before the impulse and ends after it, so the placement is
  // proved to be absolute rather than a period offset.
  signal.mix(period.data(), k_period_frames, 96);
  CHECK_EQ(at(period, 100 - 96, 3), 0x7FFFFF);
  CHECK_EQ(signal.pending(), 0u);
  CHECK_EQ(signal.fired(), 1u);

  // Every other sample of the channel, and every other channel, is untouched.
  for (unsigned frame = 0; frame < k_period_frames; ++frame) {
    if (frame != 100 - 96) {
      CHECK_EQ(at(period, frame, 3), 0);
    }
  }
  for (unsigned channel = 0; channel < k_channels; ++channel) {
    if (channel != 3) {
      CHECK_EQ(at(period, 100 - 96, channel), 0);
    }
  }
}

TEST_CASE(test_signal_a_trigger_outside_this_period_waits_for_its_own) {
  TestSignal signal;
  std::string error;
  CHECK(signal.open(make_config(0), &error));
  CHECK(signal.trigger(200, &error));

  std::vector<uint8_t> period(k_period_bytes, 0);
  signal.mix(period.data(), k_period_frames, 0);
  CHECK_EQ(signal.pending(), 1u);
  CHECK_EQ(signal.fired(), 0u);
  for (unsigned frame = 0; frame < k_period_frames; ++frame) {
    CHECK_EQ(at(period, frame, 0), 0);
  }

  signal.mix(period.data(), k_period_frames, 192);
  CHECK_EQ(signal.pending(), 0u);
  CHECK_EQ(at(period, 200 - 192, 0), 0x7FFFFF);
}

TEST_CASE(test_signal_the_same_trigger_is_the_same_bytes_twice) {
  TestSignal signal;
  std::string error;
  CHECK(signal.open(make_config(1), &error));

  std::vector<uint8_t> first(k_period_bytes, 0);
  std::vector<uint8_t> second(k_period_bytes, 0);
  // Fill both with a different programme first: "repeatable" means the mark does
  // not depend on what was underneath it.
  for (size_t sample = 0; sample < k_period_frames * k_channels; ++sample) {
    audio_bytes::put_s24(first.data() + sample * k_sample_bytes, 1234);
    audio_bytes::put_s24(second.data() + sample * k_sample_bytes, -4321);
  }
  CHECK(signal.trigger(10, &error));
  signal.mix(first.data(), k_period_frames, 0);
  CHECK(signal.trigger(10, &error));
  signal.mix(second.data(), k_period_frames, 0);
  CHECK_EQ(at(first, 10, 1), at(second, 10, 1));
  CHECK_EQ(at(first, 10, 1), 0x7FFFFF);
}

TEST_CASE(test_signal_refuses_when_no_channel_is_selected) {
  TestSignal signal;
  std::string error;
  CHECK(signal.open(make_config(-1), &error));
  CHECK(!signal.enabled());
  CHECK(!signal.trigger(10, &error));
  CHECK(!error.empty());
  // A named channel still works, so a caller can fire without a configured one.
  CHECK(signal.trigger(10, 2, &error));
  CHECK_EQ(signal.pending(), 1u);
}

TEST_CASE(test_signal_refuses_a_channel_that_does_not_exist) {
  TestSignal signal;
  std::string error;
  CHECK(signal.open(make_config(0), &error));
  CHECK(!signal.trigger(10, k_channels, &error));
  CHECK(!signal.trigger(10, -1, &error));
}

TEST_CASE(test_signal_a_full_queue_refuses_rather_than_growing) {
  TestSignal signal;
  std::string error;
  CHECK(signal.open(make_config(0), &error));
  for (unsigned i = 0; i < 8; ++i) {
    CHECK(signal.trigger(1000 + i, &error));
  }
  CHECK(!signal.trigger(2000, &error));
  CHECK_EQ(signal.missed(), 1u);
  CHECK_EQ(signal.pending(), 8u);
}
