#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "config.hpp"

namespace aes67_srt::audio {

/**
 * The shape of the AES67 audio path.
 *
 * **Bytes, not floats, and that is deliberate.** The wire format carries AES67
 * payload bytes verbatim (ADR 0001), the RAVENNA device speaks `s24_3le`, and the
 * project's first principle is that quality is never touched on the way through.
 * Converting 64 channels to float and back for no reason would be work that can
 * only lose something. The clock module converts where it genuinely needs to, and
 * nowhere else.
 */
struct AudioFormat {
  unsigned sample_rate = 48000;
  unsigned channels = 64;
  unsigned period_frames = 48;  // 1 ms at 48 kHz, AES67's packet time
  unsigned sample_bytes = 3;    // s24_3le

  /** Samples in a period: channels x frames. */
  size_t period_samples() const {
    return static_cast<size_t>(period_frames) * channels;
  }

  /** Bytes in a period, which is exactly one block's payload on the wire. */
  size_t period_bytes() const { return period_samples() * sample_bytes; }

  /** Bytes in `frames` frames. */
  size_t frames_to_bytes(size_t frames) const {
    return frames * channels * sample_bytes;
  }
};

/**
 * The AES67 audio device.
 *
 * `read()` takes audio *from* the network (the daemon's sinks, on the RAVENNA
 * capture substream) and `write()` puts audio *onto* it (the daemon's sources, on
 * the playback substream).
 *
 * Implementations are driven from one thread at a time. `read()` returns exactly
 * the frames asked for, padding with silence on underrun, so a caller never has
 * to reason about a short read mid-period.
 *
 * **A backend paces its caller, and that is part of the contract.** One period's
 * worth of wall time passes between successive reads and successive writes: the
 * engine is paced by the device, not by a timer of its own (there must be as few
 * timebases as possible — ADR 0001), so a device that returned immediately would
 * have the engine free-running at whatever rate the CPU allowed. `NullBackend`
 * implements this by waiting; a real device does it by blocking.
 *
 * **`read()` and `write()` may be called concurrently**, one thread each, because
 * the engine runs a direction per thread. Everything else — `open()`, `close()`,
 * and the counters — is the control path, and is not called concurrently with
 * them. A backend whose two directions share state must make that safe.
 */
class AudioBackend {
 public:
  virtual ~AudioBackend() = default;

  virtual bool open(const AudioFormat& format, std::string* error) = 0;
  virtual void close() = 0;
  virtual bool is_open() const = 0;

  /** Reads exactly `frames` frames of interleaved samples into `destination`. */
  virtual bool read(uint8_t* destination, unsigned frames, std::string* error) = 0;

  /** Writes exactly `frames` frames of interleaved samples from `source`. */
  virtual bool write(const uint8_t* source, unsigned frames,
                     std::string* error) = 0;

  /** Backend type: "ravenna" or "null". */
  virtual std::string kind() const = 0;

  /** Human-readable description for the log or the UI, e.g. "plughw:RAVENNA 64ch".
   */
  virtual std::string detail() const = 0;

  virtual const AudioFormat& format() const = 0;

  /** Capture xruns since the device was opened: audio the device needed and we
   *  did not have ready. */
  virtual unsigned overruns() const = 0;

  /** Playback xruns since the device was opened: audio the device wanted and we
   *  did not deliver in time. */
  virtual unsigned underruns() const = 0;
};

/**
 * Builds the backend named by `config.audio.backend`.
 *
 * A requested backend this build cannot provide is not an error until `open()`,
 * so the caller gets a reason it can show rather than a null pointer it has to
 * interpret.
 */
std::unique_ptr<AudioBackend> create_audio_backend(const AudioConfig& config);

/**
 * The runtime format a configuration asks for.
 *
 * This is the one place `audio.format` and `AudioFormat::sample_bytes` are tied
 * together, and they have to agree: the daemon's stream codec and the wire
 * format's payload type are chosen from the configuration *string*, while the
 * device and every frame-arithmetic path work in *bytes*. A disagreement
 * between the two would appear as audio of the wrong width rather than as an
 * error anywhere, which is the failure mode this project keeps designing
 * against, so the mapping lives in one function that can be tested.
 */
AudioFormat audio_format_from(const AudioConfig& config);

/** True when this build contains the ALSA/RAVENNA backend. */
bool ravenna_backend_available();

/** True when this build contains the CoreAudio backend (Apple platforms). */
bool coreaudio_backend_available();

/** True when this build contains the null backend. Always. */
bool null_backend_available();

}  // namespace aes67_srt::audio
