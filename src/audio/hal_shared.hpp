#pragma once

// The constants the plug-in and the application must agree on to meet in shared
// memory (ADR 0007, spec 0002). They live here, in one place, because a mismatch in
// any of them is a mapping that silently carries nothing.

#include <cstddef>

namespace aes67_srt::audio {

/** The POSIX shared-memory name both halves open. */
inline constexpr const char* kHalRegionName = "/aes67-srt-audio";

/** The device's channels; the link's ceiling (spec 0002, "The device"). */
inline constexpr unsigned kHalChannels = 64;

/** The region's frames per ring. Four engine periods at 20 ms, rounded to a power
 *  of two: it absorbs the cadence difference between CoreAudio's I/O cycle and the
 *  engine's period, and is not the jitter buffer (the playout buffer is). */
inline constexpr size_t kHalCapacityFrames = 4096;

}  // namespace aes67_srt::audio