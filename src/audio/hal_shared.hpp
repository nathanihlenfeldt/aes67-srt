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

/**
 * **Do not set the device's `ZeroTimeStampPeriod` to this.** It is left here as a
 * warning, not a knob.
 *
 * An earlier cut set the zero-timestamp period to a small, I/O-sized value (512) in
 * the belief that it was the HAL's I/O buffer size and that the device was running
 * I/O only once a second. It is not, and it does not: libASPL's `GetZeroTimeStamp`
 * uses the period as the timestamp ring's wrap length, and handing the HAL 512
 * while it expected the sample rate made coreaudiod spin at ~100% CPU and every
 * `system_profiler` hang. libASPL's own example devices leave it at the default
 * (the sample rate), and so do we.
 *
 * The original symptom -- a receive path that stalls with `delay 1000 ms` when
 * nothing is draining the ring -- is real and is addressed on the application side,
 * not by lying to the HAL about time.
 */
inline constexpr unsigned kHalIOPeriodFramesDoNotUse = 512;

}  // namespace aes67_srt::audio