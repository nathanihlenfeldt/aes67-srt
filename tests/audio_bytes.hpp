#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "audio/backend.hpp"

/**
 * 24-bit sample bytes, for the tests that assert on audio values.
 *
 * Three test files now need to write a known sample into a period and read one
 * back: the resampler's (the byte seam), the receive path's (the level), and the
 * engine's (the clock in the loopback). This is the third copy, so it lives in one
 * place.
 */
namespace audio_bytes {

/** One 24-bit value into a byte triple. */
inline void put_s24(uint8_t* at, int32_t value) {
  at[0] = static_cast<uint8_t>(value & 0xff);
  at[1] = static_cast<uint8_t>((value >> 8) & 0xff);
  at[2] = static_cast<uint8_t>((value >> 16) & 0xff);
}

/** The 24-bit value of one sample of a period of bytes. */
inline int32_t s24_at(const uint8_t* bytes, size_t sample) {
  const uint8_t* at = bytes + sample * 3;
  int32_t value = static_cast<int32_t>(at[0]) | (static_cast<int32_t>(at[1]) << 8) |
                  (static_cast<int32_t>(at[2]) << 16);
  if ((value & 0x800000) != 0) {
    value |= ~0xFFFFFF;
  }
  return value;
}

/**
 * A period whose every sample is |value|.
 *
 * A constant is what a resampler passes exactly — at unity gain, after its working
 * room — so it is the right test signal for a path that may resample. A period that
 * encodes its own position (which `clock_simulation.hpp` builds) is the right
 * signal for a path that must not, and the two are not interchangeable: the first
 * survives a filter, the second catches a displacement.
 */
inline std::vector<uint8_t> constant_period(
    const aes67_srt::audio::AudioFormat& format, int32_t value) {
  std::vector<uint8_t> period(format.period_bytes(), 0);
  for (size_t sample = 0; sample < format.period_frames * format.channels;
       ++sample) {
    put_s24(period.data() + sample * 3, value);
  }
  return period;
}

}  // namespace audio_bytes