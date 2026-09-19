// The application side of the endpoint's own device (issue #39): HalBackend over
// the shared-memory block. No plug-in and no CoreAudio are needed — a stand-in for
// the plug-in, on the same named region, is what the backend must work against.

#include "audio/hal_backend.hpp"

#include <cstdint>
#include <string>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#include <unistd.h>
#endif

#include "audio/backend.hpp"
#include "audio/hal_shared.hpp"
#include "audio/pcm.hpp"
#include "audio/shared_region.hpp"
#include "audio/shared_ring.hpp"
#include "config.hpp"
#include "test_framework.hpp"

namespace {

using aes67_srt::AudioConfig;
using aes67_srt::audio::AudioFormat;
using aes67_srt::audio::HalBackend;
using aes67_srt::audio::kHalCapacityFrames;
using aes67_srt::audio::kHalChannels;
using aes67_srt::audio::shared_bytes_for;
using aes67_srt::audio::SharedAudio;
using aes67_srt::audio::SharedRegion;

std::string unique_region(const char* tag) {
#if defined(__unix__) || defined(__APPLE__)
  return std::string("/a67h-") + std::to_string(getpid()) + "-" + tag;
#else
  return std::string("/a67h-") + tag;
#endif
}

AudioConfig config_64ch() {
  AudioConfig config;
  config.backend = "hal";
  config.device = "AES67-SRT";
  config.channels = static_cast<int>(kHalChannels);
  config.sample_rate = 48000;
  config.format = "s24_3le";
  config.period_frames = 960;
  return config;
}

// A stand-in for the plug-in: attaches to the same region and moves audio the way
// the real callbacks do, so the backend can be exercised without a driver.
struct FakePlugin {
  SharedRegion region;
  SharedAudio audio;
  bool ready = false;

  bool attach(const std::string& name) {
    const size_t bytes = shared_bytes_for(kHalCapacityFrames, kHalChannels);
    bool created = false;
    std::string error;
    if (!region.open(name, bytes, &created, &error) || created) {
      return false;  // the backend must have created it first
    }
    if (!audio.attach(region.data(), bytes, &error)) {
      return false;
    }
    ready = true;
    return true;
  }
};

}  // namespace

TEST_CASE(hal_backend_refuses_a_channel_count_the_device_does_not_have) {
  const std::string name = unique_region("channels");
  SharedRegion::unlink(name);
  AudioConfig config = config_64ch();
  config.channels = 8;
  HalBackend backend(config, name);
  std::string error;
  AudioFormat format;
  format.channels = 8;
  format.sample_rate = 48000;
  format.period_frames = 960;
  CHECK(!backend.open(format, &error));
  CHECK(error.find("64 channels") != std::string::npos);
}

TEST_CASE(hal_backend_refuses_a_sample_rate_the_device_does_not_run) {
  const std::string name = unique_region("rate");
  SharedRegion::unlink(name);
  HalBackend backend(config_64ch(), name);
  std::string error;
  AudioFormat format;
  format.channels = kHalChannels;
  format.sample_rate = 44100;
  format.period_frames = 960;
  CHECK(!backend.open(format, &error));
  CHECK(error.find("48000") != std::string::npos);
}

TEST_CASE(hal_backend_creates_the_region_and_the_plugin_attaches) {
  const std::string name = unique_region("create");
  SharedRegion::unlink(name);
  HalBackend backend(config_64ch(), name);
  std::string error;
  AudioFormat format;
  format.channels = kHalChannels;
  format.sample_rate = 48000;
  format.period_frames = 960;
  CHECK(backend.open(format, &error));
  CHECK(backend.is_open());
  CHECK(backend.kind() == "hal");

  FakePlugin plugin;
  CHECK(plugin.attach(name));
  SharedRegion::unlink(name);
}

TEST_CASE(hal_backend_carries_received_audio_to_the_device) {
  // write(): the engine hands the device a period of s24 bytes, and the plug-in
  // finds the matching floats. This is the site-to-DAW direction.
  const std::string name = unique_region("write");
  SharedRegion::unlink(name);
  HalBackend backend(config_64ch(), name);
  std::string error;
  AudioFormat format;
  format.channels = kHalChannels;
  format.sample_rate = 48000;
  format.period_frames = 8;  // a short period keeps the test small
  CHECK(backend.open(format, &error));

  FakePlugin plugin;
  CHECK(plugin.attach(name));

  // A ramp across the period, as s24_3le, for all 64 channels.
  const unsigned frames = format.period_frames;
  const unsigned channels = format.channels;
  std::vector<float> expected(static_cast<size_t>(frames) * channels, 0.0f);
  for (size_t i = 0; i < expected.size(); ++i) {
    expected[i] = static_cast<float>(i) / static_cast<float>(expected.size());
  }
  std::vector<uint8_t> bytes(format.frames_to_bytes(frames), 0);
  aes67_srt::audio::float_to_s24_3le(expected.data(), frames, channels,
                                     bytes.data());

  CHECK(backend.write(bytes.data(), frames, &error));

  // The plug-in reads it back as floats and it matches, to 24-bit precision.
  std::vector<float> got(static_cast<size_t>(frames) * channels, -1.0f);
  CHECK_EQ(plugin.audio.to_host().read(got.data(), frames),
           static_cast<size_t>(frames));
  for (size_t i = 0; i < got.size(); ++i) {
    CHECK(test::nearly_equal(got[i], expected[i], 1e-6));
  }
  SharedRegion::unlink(name);
}

TEST_CASE(hal_backend_takes_what_the_device_gave_us) {
  // read(): the plug-in has put audio on the input ring, and the engine gets it as
  // s24 bytes. This is the DAW-to-site direction.
  const std::string name = unique_region("read");
  SharedRegion::unlink(name);
  HalBackend backend(config_64ch(), name);
  std::string error;
  AudioFormat format;
  format.channels = kHalChannels;
  format.sample_rate = 48000;
  format.period_frames = 8;
  CHECK(backend.open(format, &error));

  FakePlugin plugin;
  CHECK(plugin.attach(name));

  const unsigned frames = format.period_frames;
  const unsigned channels = format.channels;
  std::vector<float> played(static_cast<size_t>(frames) * channels, 0.0f);
  for (size_t i = 0; i < played.size(); ++i) {
    played[i] = -0.5f + static_cast<float>(i) / static_cast<float>(played.size());
  }
  CHECK_EQ(plugin.audio.from_host().write(played.data(), frames),
           static_cast<size_t>(frames));

  std::vector<uint8_t> bytes(format.frames_to_bytes(frames), 0);
  CHECK(backend.read(bytes.data(), frames, &error));

  std::vector<float> got(static_cast<size_t>(frames) * channels, 0.0f);
  aes67_srt::audio::s24_3le_to_float(bytes.data(), frames, channels, got.data());
  for (size_t i = 0; i < got.size(); ++i) {
    CHECK(test::nearly_equal(got[i], played[i], 1e-6));
  }
  SharedRegion::unlink(name);
}

TEST_CASE(hal_backend_pads_a_starved_read_with_silence) {
  const std::string name = unique_region("starve");
  SharedRegion::unlink(name);
  HalBackend backend(config_64ch(), name);
  std::string error;
  AudioFormat format;
  format.channels = kHalChannels;
  format.sample_rate = 48000;
  format.period_frames = 8;
  CHECK(backend.open(format, &error));

  FakePlugin plugin;
  CHECK(plugin.attach(name));
  // The plug-in gives nothing; the read must still return a full period, silent.
  std::vector<uint8_t> bytes(format.frames_to_bytes(format.period_frames), 0xff);
  CHECK(backend.read(bytes.data(), format.period_frames, &error));

  std::vector<float> got(format.period_frames * format.channels, -1.0f);
  aes67_srt::audio::s24_3le_to_float(bytes.data(), format.period_frames,
                                     format.channels, got.data());
  for (float sample : got) {
    CHECK_EQ(sample, 0.0f);
  }
  CHECK(backend.underruns() > 0);
  SharedRegion::unlink(name);
}