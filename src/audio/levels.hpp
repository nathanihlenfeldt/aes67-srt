#pragma once

#include <cstdint>
#include <vector>

#include "audio/backend.hpp"
#include "config.hpp"

namespace aes67_srt::audio {

/**
 * Apply the per-block gain and mute to one period, in place.
 *
 * A block's channels appear in the device's interleaved period at
 * `BlockConfig::channels`, so this is where a level control actually acts: it
 * scales those channels and nothing else. **A block at unity gain and not muted is
 * untouched**, so the default configuration costs nothing here — the loop only
 * visits a block it has to change.
 *
 * Applied on both directions of the path, deliberately: a block's gain is a
 * property of the block, so muting block 3 silences the audio we send *and* the
 * audio we play for it. Either alone would be a surprising half-measure.
 *
 * The scaling rounds to nearest and clips rather than wrapping, exactly as the
 * resampler's byte seam does: a gain that overshoots must produce a sample at the
 * ceiling, not one of the opposite sign.
 */
void apply_block_levels(uint8_t* period, const AudioFormat& format,
                        const std::vector<BlockConfig>& blocks);

}  // namespace aes67_srt::audio
