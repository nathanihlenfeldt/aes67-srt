#include "audio/hal_backend.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>
#include <vector>

#include "audio/hal_shared.hpp"
#include "audio/pcm.hpp"
#include "audio/shared_region.hpp"
#include "audio/shared_ring.hpp"

namespace aes67_srt::audio {

namespace {

bool fail(std::string* error, const std::string& message) {
  if (error != nullptr) {
    *error = message;
  }
  return false;
}

}  // namespace

struct HalBackend::Impl {
  AudioConfig config;
  AudioFormat format;
  std::string region_name = kHalRegionName;
  bool open = false;

  SharedRegion region;
  SharedAudio audio;

  // Scratch between the ring's floats and the engine's s24 bytes, sized in open so
  // no path allocates while audio runs (read/write are called every period).
  std::vector<float> float_scratch;
  std::vector<uint8_t> byte_scratch;

  // One engine period: the rate read() and write() pace their caller.
  std::chrono::microseconds period{0};
  std::chrono::steady_clock::time_point next_read{};
  std::chrono::steady_clock::time_point next_write{};

  /**
   * Sleep until the next period boundary.
   *
   * **This is what paces the engine's receive loop, and it must not wait on the
   * device.** The loop takes exactly one period per turn and the write used to be
   * what made it so -- by waiting for room in the ring. When nothing reads the
   * device, there is never room, so each turn waited and the loop fell behind the
   * arriving audio; the playout buffer filled to its maximum and stayed there,
   * because the only thing that can drain it is a correction of a few hundred ppm.
   * A device nobody is listening to therefore froze a link that was working.
   *
   * Pacing by the clock instead is correct on macOS, not a compromise: a virtual
   * device's clock *is* the system clock, so one period of wall time is one period
   * of the device. The ring then simply overwrites when nobody is reading -- the
   * right answer for a monitor path with no listener -- and the playout buffer
   * holds its level whether or not a DAW is attached.
   *
   * If the caller falls a whole period behind, resync rather than send a burst.
   */
  void pace(std::chrono::steady_clock::time_point& deadline) {
    const auto now = std::chrono::steady_clock::now();
    if (deadline.time_since_epoch().count() == 0) {
      deadline = now;
    }
    if (now < deadline) {
      std::this_thread::sleep_until(deadline);
    } else if (now - deadline > period) {
      deadline = now;
    }
    deadline += period;
  }
};

HalBackend::HalBackend(const AudioConfig& config, std::string region_name)
    : impl_(new Impl()) {
  impl_->config = config;
  if (!region_name.empty()) {
    impl_->region_name = std::move(region_name);
  }
}

HalBackend::~HalBackend() {
  close();
}

bool HalBackend::open(const AudioFormat& format, std::string* error) {
  if (format.sample_rate == 0 || format.channels == 0 ||
      format.period_frames == 0) {
    return fail(error, "hal: a period of zero frames has no meaning");
  }
  if (format.channels != kHalChannels) {
    return fail(error, "audio.channels: the AES67-SRT device carries " +
                           std::to_string(kHalChannels) + " channels, this link " +
                           std::to_string(format.channels));
  }
  if (format.sample_rate != 48000) {
    return fail(error,
                "audio.sample_rate: the AES67-SRT device runs at 48000 Hz, "
                "this link asks for " +
                    std::to_string(format.sample_rate));
  }
  impl_->format = format;
  impl_->float_scratch.assign(
      static_cast<size_t>(format.period_frames) * format.channels, 0.0f);
  // 64 channels of 24-bit samples is the widest a period ever is; one period is the
  // most read()/write() are ever asked for.
  impl_->byte_scratch.assign(format.frames_to_bytes(format.period_frames), 0);

  // One period, the rate at which read() and write() pace the engine (see pace()).
  const double period_ms =
      static_cast<double>(format.period_frames) * 1000.0 / format.sample_rate;
  impl_->period = std::chrono::microseconds(
      static_cast<long>(std::max(1.0, period_ms) * 1000.0));

  const size_t bytes = shared_bytes_for(kHalCapacityFrames, format.channels);
  bool created = false;
  if (!impl_->region.open(impl_->region_name, bytes, &created, error)) {
    return false;
  }
  const bool attached =
      created ? impl_->audio.create(impl_->region.data(), bytes, kHalCapacityFrames,
                                    format.channels, error)
              : impl_->audio.attach(impl_->region.data(), bytes, error);
  if (!attached) {
    impl_->region.close();
    return false;
  }
  impl_->open = true;
  return true;
}

void HalBackend::close() {
  impl_->region.close();
  impl_->open = false;
}

bool HalBackend::is_open() const {
  return impl_->open;
}

bool HalBackend::read(uint8_t* destination, unsigned frames, std::string* error) {
  if (!impl_->open) {
    return fail(error, "hal: the device is not open");
  }
  if (destination == nullptr || frames == 0 ||
      frames > impl_->format.period_frames) {
    return fail(error, "hal: read of " + std::to_string(frames) +
                           " frames is not one period");
  }
  const unsigned channels = impl_->format.channels;
  SharedRing& ring = impl_->audio.from_host();
  // Pace to one period, then take whatever the device produced (or silence).
  impl_->pace(impl_->next_read);
  const size_t got = ring.read(impl_->float_scratch.data(), frames);
  if (got < frames) {
    // The device gave us less than a period. Pad with silence, as every backend
    // does on underrun, so the caller is never handed an undefined period.
    std::fill(impl_->float_scratch.begin() + static_cast<long>(got) * channels,
              impl_->float_scratch.begin() + static_cast<long>(frames) * channels,
              0.0f);
    // The ring already counted the shortfall in frames_underrun; map it to the
    // backend's underrun figure so the page reports it the same way everywhere.
  }
  float_to_s24_3le(impl_->float_scratch.data(), frames, channels, destination);
  return true;
}

bool HalBackend::write(const uint8_t* source, unsigned frames, std::string* error) {
  if (!impl_->open) {
    return fail(error, "hal: the device is not open");
  }
  if (source == nullptr || frames == 0 || frames > impl_->format.period_frames) {
    return fail(error, "hal: write of " + std::to_string(frames) +
                           " frames is not one period");
  }
  const unsigned channels = impl_->format.channels;
  s24_3le_to_float(source, frames, channels, impl_->float_scratch.data());
  SharedRing& ring = impl_->audio.to_host();
  // Pace to one period, then hand it over. No waiting for room: a device nobody is
  // reading must not slow the loop, and the ring keeps the newest and counts the
  // rest (the same never-grow-latency rule the send path uses everywhere else).
  impl_->pace(impl_->next_write);
  ring.write(impl_->float_scratch.data(), frames);
  return true;
}

std::string HalBackend::kind() const {
  return "hal";
}

std::string HalBackend::detail() const {
  return std::string("AES67-SRT shared memory ") +
         std::to_string(impl_->format.channels) + "ch @" +
         std::to_string(impl_->format.sample_rate) + "Hz (" + impl_->region_name +
         ")";
}

const AudioFormat& HalBackend::format() const {
  return impl_->format;
}

unsigned HalBackend::overruns() const {
  if (!impl_->open) {
    return 0;
  }
  return static_cast<unsigned>(impl_->audio.to_host().frames_dropped());
}

unsigned HalBackend::underruns() const {
  if (!impl_->open) {
    return 0;
  }
  return static_cast<unsigned>(impl_->audio.from_host().frames_underrun());
}

bool hal_backend_available() {
  return true;
}

}  // namespace aes67_srt::audio