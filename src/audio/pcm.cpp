#include "audio/pcm.hpp"

#include <cmath>

namespace aes67_srt::audio {
namespace {

/** 2^23: the value one LSB above full scale, so the sample range is
 *  [-8388608, 8388607] and both ends are exact in a float. */
constexpr float k_full_scale = 8388608.0f;
constexpr float k_min_sample = -8388608.0f;
constexpr float k_max_sample = 8388607.0f;

}  // namespace

void s24_3le_to_float(const uint8_t* bytes, size_t frames, unsigned channels,
                      float* samples) {
  const size_t count = frames * channels;
  for (size_t sample = 0; sample < count; ++sample) {
    const uint8_t* at = bytes + sample * 3;
    int32_t value = static_cast<int32_t>(at[0]) |
                    (static_cast<int32_t>(at[1]) << 8) |
                    (static_cast<int32_t>(at[2]) << 16);
    if ((value & 0x800000) != 0) {
      value |= ~0xFFFFFF;  // the top byte of an s24 sample is a sign
    }
    samples[sample] = static_cast<float>(value) / k_full_scale;
  }
}

void float_to_s24_3le(const float* samples, size_t frames, unsigned channels,
                      uint8_t* bytes) {
  const size_t count = frames * channels;
  for (size_t sample = 0; sample < count; ++sample) {
    float scaled = samples[sample] * k_full_scale;
    if (std::isnan(scaled)) {
      // Not a sample: silence, rather than the full-scale excursion that clipping
      // a NaN would produce.
      scaled = 0.0f;
    } else if (scaled < k_min_sample) {
      scaled = k_min_sample;
    } else if (scaled > k_max_sample) {
      scaled = k_max_sample;
    }
    const int32_t value = static_cast<int32_t>(std::lrintf(scaled));
    uint8_t* at = bytes + sample * 3;
    at[0] = static_cast<uint8_t>(value & 0xff);
    at[1] = static_cast<uint8_t>((value >> 8) & 0xff);
    at[2] = static_cast<uint8_t>((value >> 16) & 0xff);
  }
}

}  // namespace aes67_srt::audio
