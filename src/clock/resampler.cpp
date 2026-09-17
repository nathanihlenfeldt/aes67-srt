#include "clock/resampler.hpp"

#include <cmath>
#include <cstring>
#include <vector>

#if AES67_SRT_WITH_SAMPLERATE
#include <samplerate.h>
#endif

namespace aes67_srt::clock {
namespace {

bool fail(std::string* error, const std::string& message) {
  if (error != nullptr) {
    *error = message;
  }
  return false;
}

#if AES67_SRT_WITH_SAMPLERATE

int converter_type(Resampler::Converter converter) {
  switch (converter) {
    case Resampler::Converter::sinc_best:
      return SRC_SINC_BEST_QUALITY;
    case Resampler::Converter::sinc_medium:
      return SRC_SINC_MEDIUM_QUALITY;
    case Resampler::Converter::sinc_fastest:
      return SRC_SINC_FASTEST;
  }
  return SRC_SINC_MEDIUM_QUALITY;
}

std::string library_error(int code) {
  const char* text = src_strerror(code);
  if (text == nullptr) {
    return "libsamplerate error " + std::to_string(code);
  }
  return std::string(text);
}

#endif

}  // namespace

/**
 * The delay each converter adds, in frames at 48 kHz.
 *
 * **Measured as zero, which is not what this project assumed.** ADR 0003 says "a
 * resampler adds its own small delay, and the A/V delay line must know it"; what
 * the measurement in `tests/test_clock_resampler.cpp` finds is that at ratio 1 a
 * sinc converter reproduces a 1 kHz tone byte for byte at zero lag, on all three
 * converters — the polyphase implementation compensates its own filter delay. So
 * the figure the A/V line has to add for the resampler is nothing, and the table
 * says so rather than restating the assumption.
 *
 * What the converter *does* hold is look-ahead, not delay: it needs samples ahead
 * of the ones it is producing (see `carried_frames()`), which is a start-up
 * requirement rather than an offset in the played audio. At a ratio away from 1
 * the interpolation adds at most one frame of fractional offset, which is why the
 * test allows a frame either side of the table.
 *
 * The table is asserted against the measurement so that a library which changes
 * its filters fails that test instead of quietly moving the alignment.
 */
unsigned delay_for(Resampler::Converter converter) {
  switch (converter) {
    case Resampler::Converter::sinc_best:
    case Resampler::Converter::sinc_medium:
    case Resampler::Converter::sinc_fastest:
      return 0;
  }
  return 0;
}

struct Resampler::Impl {
#if AES67_SRT_WITH_SAMPLERATE
  SRC_STATE* state = nullptr;
#endif
  Resampler::Config config;
  /** Input frames consumed per output frame, from the ratio control. */
  double ratio = 1.0;
  /**
   * Frames taken from the buffer and not yet consumed, interleaved floats, with
   * the unconsumed tail kept at the front.
   *
   * libsamplerate consumes as much of the input as the ratio asks for and reports
   * how much that was, so what it did not use has to be kept: one frame dropped
   * here is one frame of audio lost, and every later output period would be
   * displaced by it.
   */
  std::vector<float> in;
  size_t carried_frames = 0;
  /** The sender's position of the first frame in |in|. */
  uint64_t pending_position = 0;
  bool primed = false;
  /** One period of output, in floats, before it becomes bytes. */
  std::vector<float> out;
  /** One period of the buffer's bytes. */
  std::vector<uint8_t> period;
  uint64_t input_frames = 0;
  uint64_t output_frames = 0;
  uint64_t short_pulls = 0;
};

Resampler::Resampler() : impl_(nullptr) {}

Resampler::~Resampler() {
  close();
}

bool Resampler::available() {
#if AES67_SRT_WITH_SAMPLERATE
  return true;
#else
  return false;
#endif
}

std::string Resampler::unavailable_reason() {
#if AES67_SRT_WITH_SAMPLERATE
  return std::string();
#else
  return "this build has no libsamplerate: install it and configure with "
         "-DWITH_SAMPLERATE=ON (see docs/research/clock-recovery.md)";
#endif
}

const char* to_string(Resampler::Converter converter) {
  switch (converter) {
    case Resampler::Converter::sinc_best:
      return "sinc_best";
    case Resampler::Converter::sinc_medium:
      return "sinc_medium";
    case Resampler::Converter::sinc_fastest:
      return "sinc_fastest";
  }
  return "unknown";
}

bool parse_converter(const std::string& text, Resampler::Converter* converter) {
  if (converter == nullptr) {
    return false;
  }
  if (text == "sinc_best") {
    *converter = Resampler::Converter::sinc_best;
  } else if (text == "sinc_medium") {
    *converter = Resampler::Converter::sinc_medium;
  } else if (text == "sinc_fastest") {
    *converter = Resampler::Converter::sinc_fastest;
  } else {
    return false;
  }
  return true;
}

bool Resampler::open(const Config& config, std::string* error) {
  close();
#if !AES67_SRT_WITH_SAMPLERATE
  return fail(error, unavailable_reason());
#else
  if (config.channels == 0 || config.rate == 0 || config.period_frames == 0) {
    // Validated configurations cannot be here; one that is would have the
    // converter working on frames that are not audio.
    return fail(error, "the resampler needs a channel count, a rate and a period");
  }

  Impl* impl = new Impl();
  impl->config = config;
  const size_t period_samples =
      static_cast<size_t>(config.period_frames) * config.channels;
  // Four periods, not two: the converter wants samples *ahead* of the ones it is
  // producing, so the carry after a pull is not the tail of one period but its own
  // look-ahead, and a filter longer than a period would need more than one. The
  // guard in fill_from_buffer refuses rather than overflowing, so a wrong
  // assumption here is a clear error and not a corruption.
  impl->in.assign(4 * period_samples, 0.0f);
  impl->out.assign(period_samples, 0.0f);
  impl->period.assign(period_samples * 3, 0);

  int code = 0;
  impl->state = src_new(converter_type(config.converter),
                        static_cast<int>(config.channels), &code);
  if (impl->state == nullptr) {
    delete impl;
    return fail(error, "libsamplerate: " + library_error(code));
  }
  impl_ = impl;
  return true;
#endif
}

void Resampler::close() {
  if (impl_ == nullptr) {
    return;
  }
#if AES67_SRT_WITH_SAMPLERATE
  if (impl_->state != nullptr) {
    src_delete(impl_->state);
    impl_->state = nullptr;
  }
#endif
  delete impl_;
  impl_ = nullptr;
}

bool Resampler::is_open() const {
  return impl_ != nullptr;
}

void Resampler::set_ratio(double ratio) {
  if (impl_ == nullptr) {
    return;
  }
  if (!std::isfinite(ratio) || ratio <= 0.0) {
    // Not a clock difference: the last good ratio is kept rather than passed on,
    // because the library's contract is a positive ratio and a NaN there would
    // produce audio that is not audio.
    return;
  }
  // Two to one either way is far outside anything a pair of crystals can be, and
  // inside it the converter is well defined. The control's own clamp is a hundred
  // times tighter than this; reaching a value this far out means something
  // upstream is broken, and a bounded ratio is the difference between strange
  // audio and no audio at all.
  if (ratio < 0.5) {
    ratio = 0.5;
  } else if (ratio > 2.0) {
    ratio = 2.0;
  }
  impl_->ratio = ratio;
}

double Resampler::ratio() const {
  return impl_ != nullptr ? impl_->ratio : 1.0;
}

uint64_t Resampler::input_frames() const {
  return impl_ != nullptr ? impl_->input_frames : 0;
}

uint64_t Resampler::output_frames() const {
  return impl_ != nullptr ? impl_->output_frames : 0;
}

uint64_t Resampler::short_pulls() const {
  return impl_ != nullptr ? impl_->short_pulls : 0;
}

uint64_t Resampler::pending_frames() const {
  return impl_ != nullptr ? impl_->carried_frames : 0;
}

uint64_t Resampler::held_frames() const {
  if (impl_ == nullptr) {
    return 0;
  }
  const double taken = static_cast<double>(impl_->output_frames) * impl_->ratio;
  const double consumed = static_cast<double>(impl_->input_frames);
  if (consumed <= taken) {
    return 0;
  }
  return static_cast<uint64_t>(consumed - taken + 0.5);
}

unsigned Resampler::delay_frames() const {
  if (impl_ == nullptr) {
    return 0;
  }
  return delay_for(impl_->config.converter);
}

bool Resampler::pull(PlayoutBuffer* buffer, uint8_t* output, unsigned frames,
                     uint64_t* position, std::string* error) {
  // A pull's reason for failing is this call's, not a previous one's: the loop
  // below tells "the buffer is dry" from "the carry is wrong" by exactly that.
  if (error != nullptr) {
    error->clear();
  }
  if (impl_ == nullptr) {
    return fail(error, "the resampler is not open");
  }
  if (buffer == nullptr || output == nullptr || frames == 0) {
    return fail(error, "the resampler was asked for no audio");
  }
  if (frames > impl_->config.period_frames) {
    // The scratch is one period, and the device's contract is one period at a
    // time. A larger request is a caller that has lost track of what a period is.
    return fail(error, "a pull cannot be larger than one period");
  }
#if !AES67_SRT_WITH_SAMPLERATE
  return fail(error, unavailable_reason());
#else
  const size_t channels = impl_->config.channels;

  // Asking the buffer for a period is what makes the carry; doing it before
  // anything is written keeps the refusal honest — a pull that returns false has
  // not touched the caller's audio.
  if (impl_->carried_frames == 0 && !fill_from_buffer(buffer, error)) {
    return false;
  }

  // Whatever is not written below is silence, which is what a device is owed when
  // the audio has not arrived. Nothing else in this class writes to |output|.
  std::memset(output, 0, frames * channels * 3);
  size_t produced = 0;
  const uint64_t started_at = impl_->primed ? impl_->pending_position : 0;

  while (produced < frames) {
    if (impl_->carried_frames == 0) {
      if (!fill_from_buffer(buffer, error)) {
        if (error != nullptr && !error->empty()) {
          return false;  // the carry is wrong rather than the buffer being dry
        }
        // The device is owed a whole period, and there is no truthful way to give
        // it less: the rest is silence, and it is counted.
        ++impl_->short_pulls;
        break;
      }
    }

    SRC_DATA data;
    std::memset(&data, 0, sizeof(data));
    data.data_in = impl_->in.data();
    data.input_frames = static_cast<long>(impl_->carried_frames);
    data.data_out = impl_->out.data();
    data.output_frames = static_cast<long>(frames - produced);
    // The library's ratio is output rate over input rate, and the control's is
    // input frames per output frame: the reciprocal, and the one place in this
    // module where getting the direction wrong is silent rather than loud.
    data.src_ratio = 1.0 / impl_->ratio;
    data.end_of_input = 0;

    const int code = src_process(impl_->state, &data);
    if (code != 0) {
      return fail(error, "libsamplerate: " + library_error(code));
    }
    if (data.output_frames_gen == 0 && data.input_frames_used == 0) {
      // The converter can do nothing with what it was given. Reachable only with a
      // filter longer than the input, so it means the configuration is wrong
      // rather than the audio.
      return fail(error, "the resampler produced nothing from " +
                             std::to_string(impl_->carried_frames) +
                             " frames of input");
    }

    // Keep what it did not consume, at the front, and remember where the sender is.
    const size_t used = static_cast<size_t>(data.input_frames_used);
    const size_t left = impl_->carried_frames - used;
    if (left > 0 && used > 0) {
      std::memmove(impl_->in.data(), impl_->in.data() + used * channels,
                   left * channels * sizeof(float));
    }
    impl_->carried_frames = left;
    impl_->pending_position += used;
    impl_->input_frames += used;

    float_to_s24_3le(impl_->out.data(), static_cast<size_t>(data.output_frames_gen),
                     channels, output + produced * channels * 3);
    produced += static_cast<size_t>(data.output_frames_gen);
    impl_->output_frames += static_cast<uint64_t>(data.output_frames_gen);
  }

  if (position != nullptr) {
    *position = started_at;
  }
  return true;
#endif
}

/**
 * Take one period from the buffer into the carry, converting it to floats.
 *
 * False when the buffer has nothing playable, in which case nothing was taken and
 * nothing was changed.
 */
bool Resampler::fill_from_buffer(PlayoutBuffer* buffer, std::string* error) {
#if AES67_SRT_WITH_SAMPLERATE
  const size_t channels = impl_->config.channels;
  const size_t period_frames = impl_->config.period_frames;
  const size_t room = impl_->in.size() / channels;
  if (impl_->carried_frames + period_frames > room) {
    // The converter is holding more input than there is room for, which means its
    // look-ahead is longer than the scratch was sized for. Refusing is the honest
    // answer: the alternative is reading past the end of a vector.
    return fail(error, "the resampler's look-ahead outgrew its scratch");
  }
  uint64_t arrived = 0;
  if (!buffer->take(impl_->period.data(), &arrived)) {
    return false;
  }
  if (!impl_->primed) {
    // The head was settled by the buffer's first period, which is the sender's
    // real position: nothing here invents position zero either.
    impl_->primed = true;
    impl_->pending_position = arrived;
  }
  s24_3le_to_float(
      impl_->period.data(), impl_->config.period_frames, impl_->config.channels,
      impl_->in.data() + impl_->carried_frames * impl_->config.channels);
  impl_->carried_frames += impl_->config.period_frames;
  return true;
#else
  (void)buffer;
  (void)error;
  return false;
#endif
}

}  // namespace aes67_srt::clock