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

  // The longest a read or a write will wait for the other side before telling the
  // truth (silence or a drop). A plug-in that has gone away must not stall the
  // engine, and a healthy one answers well inside this.
  std::chrono::microseconds patience{0};

  /**
   * Wait until `ready()` or `patience` elapses. The poll interval is short enough
   * to be indistinguishable from blocking at 48 kHz (a period is 1–20 ms) and long
   * enough not to spin a core. Returns whether `ready()` became true.
   */
  template <typename Predicate>
  bool wait_for(Predicate ready) {
    const auto deadline = std::chrono::steady_clock::now() + patience;
    if (ready()) {
      return true;
    }
    while (std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::microseconds(250));
      if (ready()) {
        return true;
      }
    }
    return ready();
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

  const double period_ms =
      static_cast<double>(format.period_frames) * 1000.0 / format.sample_rate;
  impl_->patience = std::chrono::microseconds(
      static_cast<long>(std::max(20.0, period_ms * 4.0) * 1000.0));

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
  // Paced by the plug-in: wait for it to have produced a whole period.
  impl_->wait_for([&ring, frames] { return ring.available_read() >= frames; });

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
  // Paced by the plug-in: wait for it to have consumed enough to make room.
  impl_->wait_for([&ring, frames] { return ring.available_write() >= frames; });
  // If it is still full the ring keeps the newest and counts the rest, which is the
  // same never-grow-latency rule the send path uses everywhere else.
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