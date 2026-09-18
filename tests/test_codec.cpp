#include "codec/opus.hpp"

#include <cmath>
#include <iostream>
#include <string>
#include <vector>

#include "audio/pcm.hpp"
#include "test_framework.hpp"

namespace {

using aes67_srt::codec::OpusBlock;

constexpr int kChannels = 8;
constexpr int kFrames = 960;  // 20 ms at 48 kHz
constexpr int kSampleRate = 48000;

/** A 997 Hz tone on every channel, as the wire format's interleaved s24_3le. */
std::vector<uint8_t> tone_pcm() {
  std::vector<float> samples(static_cast<size_t>(kFrames) * kChannels);
  for (int frame = 0; frame < kFrames; ++frame) {
    const double t = static_cast<double>(frame) / kSampleRate;
    const float value =
        0.25f * static_cast<float>(std::sin(2.0 * 3.14159265358979 * 997.0 * t));
    for (int channel = 0; channel < kChannels; ++channel) {
      samples[static_cast<size_t>(frame) * kChannels + channel] = value;
    }
  }
  std::vector<uint8_t> bytes(static_cast<size_t>(kFrames) * kChannels * 3);
  aes67_srt::audio::float_to_s24_3le(samples.data(), kFrames, kChannels,
                                     bytes.data());
  return bytes;
}

}  // namespace

TEST_CASE(codec_opus_round_trips_a_block) {
  if (!aes67_srt::codec::opus_available()) {
    std::cout << "    no libopus in this build: skipping the codec round trip"
              << std::endl;
    return;
  }

  OpusBlock codec;
  std::string error;
  CHECK(codec.open(kChannels, kSampleRate, kFrames, 128000, &error));
  CHECK(codec.is_open());
  // The measured figure, not the encoder's internal 4 ms constant.
  CHECK(codec.lookahead_ms() > 6.0);
  CHECK(codec.lookahead_ms() < 7.0);

  const std::vector<uint8_t> pcm = tone_pcm();
  std::vector<uint8_t> packet;
  CHECK(codec.encode(pcm.data(), kFrames, &packet, &error));
  CHECK(!packet.empty());
  // Eight channels at 128 kbit/s each over 20 ms is ~2500 bytes, and the PCM is
  // 23040: a real compression, not a copy.
  CHECK(packet.size() < pcm.size() / 4);

  std::vector<uint8_t> decoded(pcm.size());
  CHECK(
      codec.decode(packet.data(), packet.size(), kFrames, decoded.data(), &error));

  // Lossy and delayed by the codec's lookahead, so this is not a byte compare.
  // The decoded signal must be the same tone at the same level: align it by the
  // best lag (the lookahead, in samples) and require a high correlation there.
  std::vector<float> original(static_cast<size_t>(kFrames) * kChannels);
  std::vector<float> returned(static_cast<size_t>(kFrames) * kChannels);
  aes67_srt::audio::s24_3le_to_float(pcm.data(), kFrames, kChannels,
                                     original.data());
  aes67_srt::audio::s24_3le_to_float(decoded.data(), kFrames, kChannels,
                                     returned.data());

  double best = -1.0;
  int best_lag = 0;
  const int margin = 400;  // the lookahead is 312 samples; leave room
  for (int lag = 0; lag <= margin; ++lag) {
    double dot = 0.0;
    double energy_a = 0.0;
    double energy_b = 0.0;
    for (int frame = margin; frame < kFrames - margin; ++frame) {
      for (int channel = 0; channel < kChannels; ++channel) {
        const double a = original[static_cast<size_t>(frame) * kChannels + channel];
        const double b =
            returned[static_cast<size_t>(frame - lag) * kChannels + channel];
        dot += a * b;
        energy_a += a * a;
        energy_b += b * b;
      }
    }
    const double correlation = dot / std::sqrt(energy_a * energy_b + 1e-30);
    if (correlation > best) {
      best = correlation;
      best_lag = lag;
    }
  }
  // The decoder compensates the encoder's lookahead internally, so the best
  // alignment is at (or within a sample of) zero lag, and there the signal
  // matches. This is why the lookahead is a *delay to subtract from the A/V
  // budget* rather than a shift the caller has to apply to the audio.
  CHECK(best > 0.99);
  CHECK(best_lag < 100);
}

TEST_CASE(codec_opus_refuses_a_frame_it_cannot_take) {
  if (!aes67_srt::codec::opus_available()) {
    std::cout << "    no libopus in this build: skipping the frame-size refusal"
              << std::endl;
    return;
  }

  OpusBlock codec;
  std::string error;
  // 48 samples is 1 ms, below Opus's 2.5 ms minimum. Refused, with the reason.
  CHECK(!codec.open(kChannels, kSampleRate, 48, 128000, &error));
  CHECK(error.find("2.5 ms") != std::string::npos);
}
