#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "audio/backend.hpp"

namespace aes67_srt::audio {

/**
 * A device that needs no hardware, and that loops back.
 *
 * What is written to it is what it reads back, so a test can drive the whole
 * audio path — capture, framing, transmission, reassembly, playout — with no
 * ALSA device, no daemon and no network. That is how the commissioning loopback
 * is proved on a machine that has none of the three.
 *
 * It is not a mock: it is a backend with defined behaviour. A read with nothing
 * written pads with silence and counts an overrun, exactly as a real device would
 * starve, so tests can see the arithmetic rather than hope about it.
 */
class NullBackend : public AudioBackend {
 public:
  NullBackend() = default;

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

  /** Samples captured but not yet read: one period at most. */
  size_t pending_bytes() const;

 private:
  bool open_ = false;
  AudioFormat format_;
  std::vector<uint8_t> loopback_;  // holds at most one period
  size_t pending_ = 0;             // bytes of loopback_ actually carrying audio
  unsigned overruns_ = 0;
  unsigned underruns_ = 0;
};

}  // namespace aes67_srt::audio
