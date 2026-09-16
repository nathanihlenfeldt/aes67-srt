#include "audio/backend.hpp"

#include "audio/null_backend.hpp"

#if AES67_SRT_WITH_ALSA
#include "audio/ravenna_backend.hpp"
#endif

namespace aes67_srt::audio {
namespace {

bool fail(std::string* error, const std::string& message) {
  if (error != nullptr) {
    *error = message;
  }
  return false;
}

/**
 * A backend the caller asked for that this build cannot provide.
 *
 * It is a backend rather than a null pointer so the caller gets a reason it can
 * show somebody: "the wrapper returned nullptr" is not something a commissioning
 * engineer can act on, and `open()` is where the reason belongs anyway.
 */
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
#if AES67_SRT_WITH_ALSA
    return std::unique_ptr<AudioBackend>(new RavennaBackend(config));
#else
    // ALSA is a Linux-only library, so a build on a development machine or in a
    // macOS CI job genuinely has no RAVENNA path. Saying so at open() is better
    // than a null pointer the caller has to interpret.
    return std::unique_ptr<AudioBackend>(new UnavailableBackend(
        "audio.backend: this build has no ALSA support; configure with ALSA "
        "present, or use \"null\""));
#endif
  }
  return std::unique_ptr<AudioBackend>(new UnavailableBackend(
      "audio.backend: unknown backend \"" + config.backend + "\""));
}

}  // namespace aes67_srt::audio
