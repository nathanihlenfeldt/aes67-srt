#include "audio/levels.hpp"

#include <cmath>

namespace aes67_srt::audio {
namespace {

int32_t decode(const uint8_t* at, unsigned sample_bytes) {
  int32_t value = static_cast<int32_t>(at[0]) | (static_cast<int32_t>(at[1]) << 8);
  if (sample_bytes == 3) {
    value |= static_cast<int32_t>(at[2]) << 16;
    if ((value & 0x800000) != 0) {
      value |= ~0xFFFFFF;
    }
  } else if ((value & 0x8000) != 0) {
    value |= ~0xFFFF;
  }
  return value;
}

void encode(uint8_t* at, unsigned sample_bytes, int32_t value) {
  const int32_t ceiling =
      sample_bytes == 3 ? 0x7FFFFF : (sample_bytes == 2 ? 0x7FFF : 0x7F);
  const int32_t floor = -(ceiling + 1);
  if (value > ceiling) {
    value = ceiling;
  } else if (value < floor) {
    value = floor;
  }
  at[0] = static_cast<uint8_t>(value & 0xff);
  at[1] = static_cast<uint8_t>((value >> 8) & 0xff);
  if (sample_bytes == 3) {
    at[2] = static_cast<uint8_t>((value >> 16) & 0xff);
  }
}

}  // namespace

void apply_block_levels(uint8_t* period, const AudioFormat& format,
                        const std::vector<BlockConfig>& blocks) {
  if (period == nullptr) {
    return;
  }
  for (const BlockConfig& block : blocks) {
    if (!block.mute && block.gain_db == 0.0) {
      continue;  // the default path is free
    }
    const double factor = block.mute ? 0.0 : std::pow(10.0, block.gain_db / 20.0);
    for (unsigned frame = 0; frame < format.period_frames; ++frame) {
      for (size_t slot = 0; slot < block.channels.size(); ++slot) {
        const int channel = block.channels[slot];
        if (channel < 0 || channel >= static_cast<int>(format.channels)) {
          continue;
        }
        uint8_t* sample =
            period + (frame * format.channels + static_cast<size_t>(channel)) *
                         format.sample_bytes;
        const int32_t value = decode(sample, format.sample_bytes);
        const int32_t scaled =
            static_cast<int32_t>(std::lround(static_cast<double>(value) * factor));
        encode(sample, format.sample_bytes, scaled);
      }
    }
  }
}

}  // namespace aes67_srt::audio
