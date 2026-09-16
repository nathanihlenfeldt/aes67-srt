#include "audio/null_backend.hpp"

#include <algorithm>
#include <cstring>

namespace aes67_srt::audio {
namespace {

bool fail(std::string* error, const std::string& message) {
  if (error != nullptr) {
    *error = message;
  }
  return false;
}

/** A backend the caller asked for that this build cannot provide. */
class UnavailableBackend : public AudioBackend {
 public:
  explicit UnavailableBackend(std::string reason) : reason_(std::move(reason)) {}

  bool open(const AudioFormat&, std::string* error) override {
    return fail(error, reason_);
  }
  void close() override {}
  bool is_open() const override { return false; }

  bool read(uint8_t*, unsigned, std::string* error) override {
    return fail(error, reason_);
  }
  bool write(const uint8_t*, unsigned, std::string* error) override {
    return fail(error, reason_);
  }

  std::string kind() const override { return "unavailable"; }
  std::string detail() const override { return reason_; }
  const AudioFormat& format() const override { return format_; }

  unsigned overruns() const override { return 0; }
  unsigned underruns() const override { return 0; }

 private:
  std::string reason_;
  AudioFormat format_;
};

}  // namespace

bool NullBackend::open(const AudioFormat& format, std::string* error) {
  if (format.sample_rate == 0) {
    return fail(error, "audio.sample_rate: expected a rate above zero");
  }
  if (format.channels == 0) {
    return fail(error, "audio.channels: expected at least one channel");
  }
  if (format.sample_bytes != 2 && format.sample_bytes != 3) {
    return fail(error,
                "audio.format: the null backend carries 16- or 24-bit "
                "samples, not " +
                    std::to_string(format.sample_bytes) + "-byte ones");
  }
  if (format.period_frames == 0) {
    return fail(error, "audio.period_frames: expected at least one frame");
  }

  format_ = format;
  // One period of storage: the point of the device is that it holds exactly what
  // a real one would have in flight, no more.
  loopback_.assign(format_.period_bytes(), 0);
  overruns_ = 0;
  underruns_ = 0;
  open_ = true;
  return true;
}

void NullBackend::close() {
  open_ = false;
  pending_ = 0;
  loopback_.clear();
}

bool NullBackend::is_open() const {
  return open_;
}

bool NullBackend::write(const uint8_t* source, unsigned frames,
                        std::string* error) {
  if (!open_) {
    return fail(error, "the null backend is not open");
  }
  if (source == nullptr) {
    return fail(error, "no audio to write");
  }
  const size_t bytes = format_.frames_to_bytes(frames);
  if (bytes > loopback_.size()) {
    // A device that cannot take what it was given is exactly the condition the
    // underrun counter exists to report, so it is counted rather than swallowed.
    ++underruns_;
    std::memcpy(loopback_.data(), source, loopback_.size());
    pending_ = loopback_.size();
    return true;
  }
  std::memcpy(loopback_.data(), source, bytes);
  pending_ = bytes;
  return true;
}

bool NullBackend::read(uint8_t* destination, unsigned frames, std::string* error) {
  if (!open_) {
    return fail(error, "the null backend is not open");
  }
  if (destination == nullptr) {
    return fail(error, "no buffer to read into");
  }
  const size_t bytes = format_.frames_to_bytes(frames);

  // Silence first, then whatever is waiting. A caller always gets a whole period:
  // a short read mid-period would push the arithmetic of starvation into every
  // caller instead of counting it here, where it can be seen.
  std::memset(destination, 0, bytes);
  if (pending_ < bytes) {
    ++overruns_;
  }
  const size_t available = std::min(pending_, bytes);
  if (available > 0) {
    std::memcpy(destination, loopback_.data(), available);
    std::fill(loopback_.begin(), loopback_.end(), 0);
    pending_ = 0;
  }
  return true;
}

std::string NullBackend::kind() const {
  return "null";
}

std::string NullBackend::detail() const {
  return "null device, " + std::to_string(format_.channels) + "ch at " +
         std::to_string(format_.sample_rate) + " Hz (loops back what is written)";
}

const AudioFormat& NullBackend::format() const {
  return format_;
}

unsigned NullBackend::overruns() const {
  return overruns_;
}

unsigned NullBackend::underruns() const {
  return underruns_;
}

size_t NullBackend::pending_bytes() const {
  return pending_;
}

bool ravenna_backend_available() {
#if AES67_SRT_WITH_ALSA
  return true;
#else
  return false;
#endif
}

bool null_backend_available() {
  return true;
}

std::unique_ptr<AudioBackend> create_audio_backend(const AudioConfig& config) {
  if (config.backend == "null") {
    return std::unique_ptr<AudioBackend>(new NullBackend());
  }
  if (config.backend == "ravenna") {
    if (ravenna_backend_available()) {
      // The ALSA backend lands with ticket 09's second slice; until it exists this
      // build has no ALSA path, and saying so is better than a null pointer the
      // caller has to interpret.
      return std::unique_ptr<AudioBackend>(new UnavailableBackend(
          "audio.backend: the ALSA/RAVENNA backend is not built into this binary "
          "yet"));
    }
    return std::unique_ptr<AudioBackend>(new UnavailableBackend(
        "audio.backend: this build has no ALSA support; configure with ALSA "
        "present, or use \"null\""));
  }
  return std::unique_ptr<AudioBackend>(new UnavailableBackend(
      "audio.backend: unknown backend \"" + config.backend + "\""));
}

}  // namespace aes67_srt::audio
