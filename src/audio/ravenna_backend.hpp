#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <shared_mutex>
#include <string>

#include <alsa/asoundlib.h>

#include "audio/backend.hpp"
#include "config.hpp"

namespace aes67_srt::audio {

/**
 * The RAVENNA device, through ALSA.
 *
 * The kernel module exposes one PCM device (`plughw:RAVENNA` by default) whose
 * channels are bound to AES67 streams by the daemon's channel maps, so this
 * backend only has to behave like a plain 64-channel sound card at the AES67
 * native rate:
 *
 *   capture  substream -> audio arriving on the daemon's sinks
 *   playback substream -> audio going out on the daemon's sources
 *
 * **Bytes, not floats**, unlike the sibling appliance: the samples are AES67
 * payload bytes (ADR 0001) and the device takes `s24_3le`, so they are handed to
 * ALSA and taken back verbatim. Nothing between the device and the wire
 * converts them, which is the whole point of the frame format carrying payload
 * bytes.
 *
 * Two behaviours here are not obvious and were learned on hardware, not read in
 * a manual — both are carried over from `aes67-sip`, which runs on this same
 * daemon and this same driver. Read the comments on `start_stream` and
 * `recover_if_stalled` before changing either.
 */
class RavennaBackend : public AudioBackend {
 public:
  explicit RavennaBackend(const AudioConfig& config);
  ~RavennaBackend() override;

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

  /** The device name as configured, e.g. "plughw:RAVENNA". */
  const std::string& device() const;

 private:
  bool open_stream(snd_pcm_stream_t direction, snd_pcm_t** handle,
                   std::string* error);

  /**
   * Trigger a substream so the driver's audio engine starts ticking.
   *
   * The RAVENNA module only exchanges audio while a substream is *triggered*:
   * its ALSA trigger callback marks the direction as running, and the driver's
   * 1 ms audio tick only copies frames between the ALSA rings and the RTP
   * streams when that flag is set. A prepared-but-never-started substream
   * therefore leaves its pointer at zero for ever — no capture reaches us, our
   * sources transmit nothing, and the audio the daemon's sinks receive is never
   * played out, with no error anywhere. Waiting for data cannot recover from
   * that, because the device has to be started first.
   *
   * For playback the ring is primed with silence first: an empty playback ring
   * underruns on the very first tick and the driver stops the stream again.
   */
  bool start_stream(snd_pcm_t* handle, std::string* error);

  /**
   * Reopen the device when the driver's engine has gone idle.
   *
   * The RAVENNA module stops its 1 ms engine when the daemon tells it to
   * restart — starting or restarting `aes67-daemon` does that, as does a sample
   * rate change. Both substreams stay open and report RUNNING, but not one frame
   * moves: nothing is captured, our sources transmit nothing and the far end
   * hears nothing, with no error anywhere. Closing and reopening the PCM
   * re-triggers the streams, so that is what this does, throttled.
   */
  void recover_if_stalled();

  void fill_silence(uint8_t* destination, unsigned frames) const;

  AudioConfig config_;
  AudioFormat format_{};
  snd_pcm_format_t pcm_format_{SND_PCM_FORMAT_S16_LE};

  snd_pcm_t* capture_{nullptr};
  snd_pcm_t* playback_{nullptr};

  /** Negotiated playback geometry, used to prime the ring before triggering. */
  snd_pcm_uframes_t playback_period_frames_{0};
  snd_pcm_uframes_t playback_buffer_frames_{0};

  /** Monotonic seconds of the last captured frame, and of the last reopen. */
  double last_frames_at_{0.0};
  double last_recover_at_{0.0};

  /**
   * Guards the substreams against the one thing that touches both of them.
   *
   * `read()` and `write()` take it *shared*, so the two directions run on their
   * own threads without waiting for each other — which is the whole reason they
   * have a thread each. `recover_if_stalled()` takes it *exclusive*, because it
   * closes and reopens *both* substreams: without this, a reopen on the capture
   * thread would free the playback handle while the playback thread was inside a
   * call on it. That is not a theoretical race — the stall it recovers from
   * happens every time the daemon restarts.
   *
   * `open()` and `close()` deliberately do not take it: they belong to the
   * control path, which the engine uses before the threads start and after they
   * have joined. Recovery is the one exception and holds it on their behalf.
   */
  mutable std::shared_mutex path_mutex_;

  /** Read from the status page, so atomic; see the same note in NullBackend. */
  std::atomic<unsigned> overruns_{0};
  std::atomic<unsigned> underruns_{0};
  bool open_{false};
};

}  // namespace aes67_srt::audio
