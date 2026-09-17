#pragma once

#include <cstddef>
#include <cstdint>

namespace aes67_srt::audio {

/**
 * s24_3le bytes <-> interleaved 32-bit float in [-1, 1), and back.
 *
 * **This is the one place the project meets a float-native platform, and it is
 * deliberately not inside a realtime callback.** ADR 0005's macOS endpoint is
 * float32 all the way to the ring, so the conversion to the wire format's bytes
 * happens here, on the engine's side, as ordinary testable code.
 *
 * Both directions are exact for 24-bit audio: every such integer is representable
 * in a float, and the way back rounds to nearest and clips rather than wrapping —
 * a converter may overshoot, and an overshoot that wrapped would be a click the
 * size of the signal instead of a sample at the ceiling. A NaN becomes silence
 * rather than a full-scale excursion.
 *
 * It lives in `audio` rather than in the clock because the clock's resampler is
 * only one caller: a device backend needs it too, and a backend that depends on
 * the clock would point the dependency the wrong way.
 */
void s24_3le_to_float(const uint8_t* bytes, size_t frames, unsigned channels,
                      float* samples);
void float_to_s24_3le(const float* samples, size_t frames, unsigned channels,
                      uint8_t* bytes);

}  // namespace aes67_srt::audio
