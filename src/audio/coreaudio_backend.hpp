#pragma once

#include <memory>
#include <string>

#include "audio/backend.hpp"
#include "config.hpp"

namespace aes67_srt::audio {

/**
 * The macOS endpoint's device: an AudioUnit on a CoreAudio device (ADR 0005).
 *
 * This is the second product's `AudioBackend`, and it is deliberately the same
 * shape as the appliance's: the engine sees bytes and a period, and knows nothing
 * about CoreAudio. What is different is the direction of control — **the device
 * calls us**, at its own I/O cycle, through a render callback — so between that
 * callback and the engine's ordinary blocking `read()`/`write()` sit two lock-free
 * rings of 32-bit float (`audio::FloatRing`).
 *
 * - `write()` is this process handing audio **to** the device: it becomes float and
 *   goes into the output ring, which the render callback drains. On a loopback
 *   device a DAW reads it from the device's input side.
 * - `read()` is audio the device **gave us**: the input callback fills the input
 *   ring and `read()` converts it back to the wire format's bytes.
 * - The float ↔ `s24_3le` conversion happens here, on the engine's side of the
 *   ring, in ordinary code — never inside the callback, which only copies.
 *
 * The device is chosen by name from `audio.device`, or the system default output
 * when it is empty. A device that is missing, or that cannot carry the configured
 * channel count in the direction asked for, is refused by name rather than opened
 * and found wanting later.
 */
class CoreAudioBackend : public AudioBackend {
 public:
  explicit CoreAudioBackend(const AudioConfig& config);
  ~CoreAudioBackend() override;

  CoreAudioBackend(const CoreAudioBackend&) = delete;
  CoreAudioBackend& operator=(const CoreAudioBackend&) = delete;

  bool open(const AudioFormat& format, std::string* error) override;
  void close() override;
  bool is_open() const override;

  bool read(uint8_t* destination, unsigned frames, std::string* error) override;
  bool write(const uint8_t* source, unsigned frames, std::string* error) override;

  std::string kind() const override;
  std::string detail() const override;
  const AudioFormat& format() const override;

  unsigned overruns() const override;
  unsigned underruns() const override;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace aes67_srt::audio
