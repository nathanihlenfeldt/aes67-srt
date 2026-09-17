#include "delay/delay_line.hpp"

#include <cmath>
#include <cstring>

namespace aes67_srt::delay {
namespace {

constexpr double k_pi = 3.14159265358979323846;

bool fail(std::string* error, const std::string& message) {
  if (error != nullptr) {
    *error = message;
  }
  return false;
}

/** Sign-extend |sample_bytes| little-endian bytes into an int32. */
int32_t decode_sample(const uint8_t* at, unsigned sample_bytes) {
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

/** Write an int32 as |sample_bytes| little-endian bytes, clipped rather than
 * wrapped. */
void encode_sample(uint8_t* at, unsigned sample_bytes, int32_t value) {
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

DelayLine::DelayLine() = default;
DelayLine::~DelayLine() = default;

bool DelayLine::open(const Config& config, std::string* error) {
  if (config.channels == 0) {
    return fail(error, "delay: no channels");
  }
  if (config.sample_bytes != 2 && config.sample_bytes != 3) {
    return fail(error, "delay: sample_bytes must be 2 or 3, got " +
                           std::to_string(config.sample_bytes));
  }
  if (config.period_frames == 0 || config.sample_rate == 0) {
    return fail(error, "delay: a period of zero frames has no meaning");
  }
  if (!(config.capacity_ms >= 0.0)) {
    return fail(error, "delay: capacity_ms must not be negative");
  }
  if (config.fade_ms < 0.0) {
    return fail(error, "delay: fade_ms must not be negative");
  }

  config_ = config;
  sample_bytes_ = config.sample_bytes;
  channels_ = config.channels;
  max_delay_frames_ =
      static_cast<uint64_t>(config.capacity_ms * config.sample_rate / 1000.0 + 0.5);
  fade_frames_ =
      static_cast<unsigned>(config.fade_ms * config.sample_rate / 1000.0 + 0.5);
  // One period of margin, because a period is written before it is read, and one
  // more frame so the ring is always strictly larger than the span it holds.
  ring_frames_ = max_delay_frames_ + fade_frames_ + config.period_frames + 2;
  ring_.assign(static_cast<size_t>(ring_frames_) * channels_ * sample_bytes_, 0);

  written_frames_ = 0;
  applied_delay_ = 0;
  target_delay_ = 0;
  fading_ = false;
  fade_from_ = 0;
  fade_to_ = 0;
  fade_step_ = 0;
  pushed_frames_ = 0;
  crossfades_ = 0;
  return true;
}

void DelayLine::close() {
  ring_.clear();
  ring_.shrink_to_fit();
  ring_frames_ = 0;
  channels_ = 0;
  written_frames_ = 0;
  fading_ = false;
}

bool DelayLine::is_open() const {
  return !ring_.empty();
}

bool DelayLine::set_offset_ms(double offset_ms, std::string* error) {
  if (!is_open()) {
    return fail(error, "delay: not open");
  }
  const double capacity_ms =
      static_cast<double>(max_delay_frames_) * 1000.0 / config_.sample_rate;
  if (!(offset_ms >= 0.0) || offset_ms > capacity_ms) {
    return fail(error, "delay: offset " + std::to_string(offset_ms) +
                           " ms is outside 0.." + std::to_string(capacity_ms) +
                           " ms (audio can be delayed, never advanced)");
  }
  uint64_t frames =
      static_cast<uint64_t>(offset_ms * config_.sample_rate / 1000.0 + 0.5);
  if (frames > max_delay_frames_) {
    frames = max_delay_frames_;
  }
  target_delay_ = frames;
  if (pushed_frames_ == 0 && !fading_) {
    // The offset a line is *opened* with is not a change: there is nothing to
    // crossfade from. Applying it immediately is what turns "ask for 500 ms" into
    // 500 ms of silence while the line fills, rather than a fade out of audio it
    // does not have.
    applied_delay_ = frames;
  }
  return true;
}

double DelayLine::offset_ms() const {
  return static_cast<double>(target_delay_) * 1000.0 / config_.sample_rate;
}

double DelayLine::applied_offset_ms() const {
  return static_cast<double>(applied_delay_) * 1000.0 / config_.sample_rate;
}

bool DelayLine::adjusting() const {
  return fading_;
}

double DelayLine::fade_weight(unsigned step) const {
  if (fade_frames_ == 0) {
    return 1.0;
  }
  if (step >= fade_frames_) {
    return 1.0;
  }
  const double t = static_cast<double>(step) / static_cast<double>(fade_frames_);
  return 0.5 * (1.0 - std::cos(k_pi * t));
}

int32_t DelayLine::sample_at(uint64_t frame, unsigned channel) const {
  const uint64_t ring_samples = ring_frames_ * channels_;
  const uint64_t index = (frame * channels_ + channel) % ring_samples;
  return decode_sample(&ring_[index * sample_bytes_], sample_bytes_);
}

void DelayLine::copy_out(int64_t first, unsigned frames, uint8_t* out) const {
  const size_t frame_bytes = static_cast<size_t>(channels_) * sample_bytes_;
  std::memset(out, 0, static_cast<size_t>(frames) * frame_bytes);
  if (first + static_cast<int64_t>(frames) <= 0) {
    return;  // the whole period is before the line has heard anything
  }
  // The part of the period that exists: skip the frames before frame zero.
  uint64_t start = first < 0 ? 0 : static_cast<uint64_t>(first);
  const size_t skip = first < 0 ? static_cast<size_t>(-first) : 0;
  const unsigned count = frames - static_cast<unsigned>(skip);

  const uint64_t ring_samples = ring_frames_ * channels_;
  const uint64_t first_sample = start * channels_;
  const size_t samples = static_cast<size_t>(count) * channels_;
  const size_t offset = static_cast<size_t>(first_sample % ring_samples);
  uint8_t* destination = out + skip * frame_bytes;
  if (offset + samples <= ring_samples) {
    std::memcpy(destination, &ring_[offset * sample_bytes_],
                samples * sample_bytes_);
    return;
  }
  const size_t head = ring_samples - offset;
  std::memcpy(destination, &ring_[offset * sample_bytes_], head * sample_bytes_);
  std::memcpy(destination + head * sample_bytes_, ring_.data(),
              (samples - head) * sample_bytes_);
}

bool DelayLine::process(const uint8_t* input, uint8_t* output, unsigned frames,
                        std::string* error) {
  if (!is_open()) {
    return fail(error, "delay: not open");
  }
  if (input == nullptr || output == nullptr) {
    return fail(error, "delay: no audio");
  }
  if (frames == 0 || frames > config_.period_frames) {
    return fail(error, "delay: a process call carries 1.." +
                           std::to_string(config_.period_frames) + " frames");
  }

  // The input goes in first, so an offset of zero reads back the period just
  // written — which is what makes zero a memcpy rather than a period of latency.
  const uint64_t ring_samples = ring_frames_ * channels_;
  {
    const uint64_t first_sample = written_frames_ * channels_;
    const size_t samples = static_cast<size_t>(frames) * channels_;
    const size_t offset = static_cast<size_t>(first_sample % ring_samples);
    if (offset + samples <= ring_samples) {
      std::memcpy(&ring_[offset * sample_bytes_], input, samples * sample_bytes_);
    } else {
      const size_t head = ring_samples - offset;
      std::memcpy(&ring_[offset * sample_bytes_], input, head * sample_bytes_);
      std::memcpy(ring_.data(), input + head * sample_bytes_,
                  (samples - head) * sample_bytes_);
    }
  }
  written_frames_ += frames;
  pushed_frames_ += frames;
  const int64_t read_base = static_cast<int64_t>(written_frames_) - frames;

  // A change requested since the last period is where a fade begins. One that
  // arrives while a fade is running is not lost: `target_delay_` already holds
  // the newest request, and the next turn starts a fade toward it after this one
  // lands.
  if (!fading_ && target_delay_ != applied_delay_) {
    fading_ = true;
    fade_from_ = applied_delay_;
    fade_to_ = target_delay_;
    fade_step_ = 0;
  }

  size_t done = 0;
  if (fading_) {
    const size_t frame_bytes = static_cast<size_t>(channels_) * sample_bytes_;
    for (; done < frames && fading_; ++done) {
      const double weight = fade_weight(fade_step_);
      const int64_t from_frame = read_base - static_cast<int64_t>(fade_from_);
      const int64_t to_frame = read_base - static_cast<int64_t>(fade_to_);
      uint8_t* out = output + done * frame_bytes;
      for (unsigned channel = 0; channel < channels_; ++channel) {
        const int64_t a_frame = from_frame + static_cast<int64_t>(done);
        const int64_t b_frame = to_frame + static_cast<int64_t>(done);
        const int32_t a = a_frame < 0 ? 0 : sample_at(a_frame, channel);
        const int32_t b = b_frame < 0 ? 0 : sample_at(b_frame, channel);
        const double blended = static_cast<double>(a) * (1.0 - weight) +
                               static_cast<double>(b) * weight;
        encode_sample(out + channel * sample_bytes_, sample_bytes_,
                      static_cast<int32_t>(std::lround(blended)));
      }
      ++fade_step_;
      if (fade_step_ > fade_frames_) {
        fading_ = false;
        applied_delay_ = fade_to_;
        ++crossfades_;
      }
    }
  }

  if (done < frames) {
    copy_out(read_base - static_cast<int64_t>(applied_delay_) + done, frames - done,
             output + done * static_cast<size_t>(channels_) * sample_bytes_);
  }
  return true;
}

uint64_t DelayLine::pushed_frames() const {
  return pushed_frames_;
}

uint64_t DelayLine::crossfades() const {
  return crossfades_;
}

}  // namespace aes67_srt::delay
