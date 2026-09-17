#include "audio/ravenna_backend.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <vector>

#include "log.hpp"

namespace aes67_srt::audio {
namespace {

/** How long a read or write waits for the device before giving up on it. */
constexpr int k_wait_timeout_ms = 500;

/** No captured frames for this long means the RAVENNA engine stopped ticking. */
constexpr double k_stall_seconds = 2.0;

/** Do not reopen the device more often than this while it stays idle. */
constexpr double k_recover_interval_seconds = 15.0;

/**
 * The smallest buffer we ask the driver for, in periods.
 *
 * The device is clocked at 1 ms periods, and on kernels from 6.15 the driver's
 * tick is a soft hrtimer that delivers audio in bursts: a three-period buffer
 * overruns as soon as the thread is delayed by anything else on the machine.
 * `audio.periods` defaults to eight, and six is the floor. The bench Pi runs
 * 6.18.34, so this is not hypothetical — it is why the default is eight.
 */
constexpr unsigned k_min_periods = 6;

snd_pcm_format_t alsa_format(unsigned sample_bytes) {
  return sample_bytes == 2 ? SND_PCM_FORMAT_S16_LE : SND_PCM_FORMAT_S24_3LE;
}

/** Monotonic seconds: stall detection must not follow the wall clock. */
double mono_seconds() {
  return std::chrono::duration<double>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

bool fail(std::string* error, const std::string& message) {
  if (error != nullptr) {
    *error = message;
  }
  return false;
}

}  // namespace

RavennaBackend::RavennaBackend(const AudioConfig& config) : config_(config) {}

RavennaBackend::~RavennaBackend() {
  close();
}

bool RavennaBackend::open(const AudioFormat& format, std::string* error) {
  if (format.sample_rate == 0) {
    return fail(error, "audio.sample_rate: expected a rate above zero");
  }
  if (format.channels == 0) {
    return fail(error, "audio.channels: expected at least one channel");
  }
  if (format.period_frames == 0) {
    return fail(error, "audio.period_frames: expected at least one frame");
  }
  if (format.sample_bytes != 2 && format.sample_bytes != 3) {
    return fail(error,
                "audio.format: the RAVENNA device carries 16- or 24-bit "
                "samples, not " +
                    std::to_string(format.sample_bytes) + "-byte ones");
  }

  format_ = format;
  pcm_format_ = alsa_format(format.sample_bytes);

  // Deliberately no s16_le fallback here, which is where this backend parts
  // company with its sibling.
  //
  // aes67-sip retries a device that refuses S24_3LE at 16 bits, and for it that
  // is invisible: its samples become floats and the AES67 payload is the
  // daemon's business. Here the sample width *is* the wire contract. It comes
  // from `audio.format`, which selects the frame's payload type (pcm_l24) and
  // the daemon's stream codec (L24), and the frame carries these bytes verbatim
  // (ADR 0001). Capturing 16-bit samples from a device we asked for 24 and
  // framing them as L24 would be the wrong audio with nothing anywhere
  // reporting it. A device that refuses the configured format is therefore a
  // commissioning error to fix: change `audio.format`, and the payload type and
  // the daemon's codec follow it in one move.
  if (!open_stream(SND_PCM_STREAM_CAPTURE, &capture_, error)) {
    close();
    return false;
  }
  if (!open_stream(SND_PCM_STREAM_PLAYBACK, &playback_, error)) {
    close();
    return false;
  }

  open_ = true;
  last_frames_at_ = mono_seconds();
  last_recover_at_ = 0.0;
  log().write(LogLevel::info, "RAVENNA audio device: " + detail());
  return true;
}

void RavennaBackend::close() {
  if (capture_ != nullptr) {
    snd_pcm_close(capture_);
    capture_ = nullptr;
  }
  if (playback_ != nullptr) {
    snd_pcm_close(playback_);
    playback_ = nullptr;
  }
  playback_period_frames_ = 0;
  playback_buffer_frames_ = 0;
  open_ = false;
}

bool RavennaBackend::is_open() const {
  return open_;
}

bool RavennaBackend::open_stream(snd_pcm_stream_t direction, snd_pcm_t** handle,
                                 std::string* error) {
  const bool capture = direction == SND_PCM_STREAM_CAPTURE;
  const std::string what = capture ? "capture" : "playback";

  int rc = snd_pcm_open(handle, config_.device.c_str(), direction, 0);
  if (rc < 0) {
    return fail(error, "cannot open " + what + " device '" + config_.device +
                           "': " + snd_strerror(rc));
  }

  snd_pcm_hw_params_t* params = nullptr;
  snd_pcm_hw_params_alloca(&params);
  if ((rc = snd_pcm_hw_params_any(*handle, params)) < 0) {
    return fail(error, "cannot read hw params of '" + config_.device +
                           "': " + snd_strerror(rc));
  }
  if ((rc = snd_pcm_hw_params_set_access(*handle, params,
                                         SND_PCM_ACCESS_RW_INTERLEAVED)) < 0) {
    return fail(error, "interleaved access not supported on '" + config_.device +
                           "': " + snd_strerror(rc));
  }
  if ((rc = snd_pcm_hw_params_set_format(*handle, params, pcm_format_)) < 0) {
    // Name the format we asked for and the configuration field that would
    // change it: this is the error a commissioning engineer is most likely to
    // see on a device that is present and working.
    return fail(error, "sample format '" +
                           std::string(snd_pcm_format_name(pcm_format_)) +
                           "' not supported on '" + config_.device +
                           "': " + snd_strerror(rc) + " (changed by audio.format)");
  }
  unsigned channels = format_.channels;
  if ((rc = snd_pcm_hw_params_set_channels(*handle, params, channels)) < 0) {
    return fail(error, "cannot set " + std::to_string(channels) + " channels on '" +
                           config_.device + "': " + snd_strerror(rc) +
                           " (check the ravenna-alsa-lkm max_channels module "
                           "parameter)");
  }

  unsigned rate = format_.sample_rate;
  int rate_direction = 0;
  if ((rc = snd_pcm_hw_params_set_rate_near(*handle, params, &rate,
                                            &rate_direction)) < 0) {
    return fail(error, "cannot set " + std::to_string(format_.sample_rate) +
                           " Hz on '" + config_.device + "': " + snd_strerror(rc));
  }
  if (rate != format_.sample_rate) {
    // Worth a warning rather than a failure: the driver runs at its own PTP-derived
    // rate, and if it is not the rate the AES67 streams use, everything downstream
    // is at the wrong speed rather than broken.
    log().write(LogLevel::warn,
                "device '" + config_.device + "' runs at " + std::to_string(rate) +
                    " Hz instead of the requested " +
                    std::to_string(format_.sample_rate) +
                    " Hz - the AES67 clock and the driver sample rate must match");
  }

  int period_direction = 0;
  snd_pcm_uframes_t period = format_.period_frames;
  if ((rc = snd_pcm_hw_params_set_period_size_near(*handle, params, &period,
                                                   &period_direction)) < 0) {
    return fail(error, "cannot set period size on '" + config_.device +
                           "': " + snd_strerror(rc));
  }
  snd_pcm_uframes_t buffer =
      period * std::max(k_min_periods, static_cast<unsigned>(config_.periods));
  snd_pcm_hw_params_set_buffer_size_near(*handle, params, &buffer);

  if ((rc = snd_pcm_hw_params(*handle, params)) < 0) {
    return fail(error, "cannot apply hw params on '" + config_.device +
                           "': " + snd_strerror(rc));
  }
  if ((rc = snd_pcm_prepare(*handle)) < 0) {
    return fail(error,
                "cannot prepare '" + config_.device + "': " + snd_strerror(rc));
  }

  // The driver is free to give something other than what was asked for, so read
  // back what it actually negotiated rather than assuming.
  snd_pcm_uframes_t period_frames = period;
  snd_pcm_uframes_t buffer_frames = buffer;
  snd_pcm_get_params(*handle, &buffer_frames, &period_frames);
  if (!capture) {
    playback_period_frames_ = period_frames;
    playback_buffer_frames_ = buffer_frames;
  }
  if (!start_stream(*handle, error)) {
    return false;
  }

  log().write(LogLevel::debug,
              what + " stream open: period " + std::to_string(period_frames) +
                  " frames, buffer " + std::to_string(buffer_frames));
  return true;
}

bool RavennaBackend::start_stream(snd_pcm_t* handle, std::string* error) {
  if (handle == nullptr) {
    return false;
  }
  const bool playback = snd_pcm_stream(handle) == SND_PCM_STREAM_PLAYBACK;

  if (playback) {
    // Prime the ring with silence. An empty playback ring underruns on the first
    // tick after it is triggered, and the driver stops the stream again, so the
    // stream would be started and immediately dead.
    const snd_pcm_uframes_t prime = playback_buffer_frames_;
    if (prime > 0) {
      const size_t bytes = format_.frames_to_bytes(prime);
      const std::vector<uint8_t> silence(bytes, 0);
      const snd_pcm_sframes_t written =
          snd_pcm_writei(handle, silence.data(), prime);
      if (written < 0) {
        // Not fatal: the stream is started below either way, and a driver that
        // refuses the priming write is one whose ring was already full.
        log().write(LogLevel::debug,
                    "playback prime write: " + std::string(snd_strerror(written)));
      }
    }
  }

  if (snd_pcm_state(handle) == SND_PCM_STATE_PREPARED) {
    const int rc = snd_pcm_start(handle);
    if (rc < 0) {
      return fail(error, "cannot start '" + config_.device + "' (" +
                             (playback ? "playback" : "capture") +
                             "): " + snd_strerror(rc));
    }
  }
  return true;
}

void RavennaBackend::recover_if_stalled() {
  if (!open_) {
    return;
  }
  const double now = mono_seconds();
  if (last_frames_at_ > 0.0 && now - last_frames_at_ < k_stall_seconds) {
    return;
  }
  if (last_recover_at_ > 0.0 &&
      now - last_recover_at_ < k_recover_interval_seconds) {
    return;
  }
  last_recover_at_ = now;

  // Exclusive, because this closes and reopens *both* substreams: the other
  // direction's thread must not be inside a call on a handle being freed.
  std::unique_lock<std::shared_mutex> lock(path_mutex_);

  // Reopening the PCM re-triggers both substreams, which is what makes the
  // driver's engine run again after the daemon restarted it underneath us. The
  // streams keep reporting RUNNING the whole time, so this is the only way to
  // tell, and the throttle is what stops a device that is idle for a real reason
  // (nothing is subscribed to it yet) from being reopened for ever.
  log().write(LogLevel::warn,
              "no audio from '" + config_.device + "' for " +
                  std::to_string(static_cast<int>(now - last_frames_at_)) +
                  " s: the RAVENNA engine looks stopped (restarting aes67-daemon "
                  "does that) - reopening the device");
  const AudioFormat format = format_;
  close();
  std::string error;
  if (!open(format, &error)) {
    log().write(LogLevel::error,
                "cannot reopen '" + config_.device + "': " + error);
  }
}

void RavennaBackend::fill_silence(uint8_t* destination, unsigned frames) const {
  std::memset(destination, 0, format_.frames_to_bytes(frames));
}

bool RavennaBackend::read(uint8_t* destination, unsigned frames,
                          std::string* error) {
  if (!open_ || capture_ == nullptr) {
    return fail(error, "the RAVENNA capture device is not open");
  }
  if (destination == nullptr) {
    return fail(error, "no buffer to read into");
  }

  const size_t frame_bytes = format_.frames_to_bytes(1);
  unsigned remaining = frames;
  size_t offset = 0;
  bool stalled = false;
  {
    // Shared, not exclusive: the two directions have a thread each so that
    // neither waits for the other. Recovery takes this exclusively, and is
    // called *after* this scope ends — upgrading it on this thread would
    // deadlock against itself.
    std::shared_lock<std::shared_mutex> lock(path_mutex_);
    while (remaining > 0) {
      const int ready = snd_pcm_wait(capture_, k_wait_timeout_ms);
      if (ready < 0) {
        if (ready == -EPIPE) {
          // The **wait** reports the overrun before `readi` can, so the recovery
          // below never ran and the device stayed XRUN for ever -- which is how a
          // listener that opened the device and then waited for a caller (see
          // Engine::open) ended up unable to produce a single frame. Recover here
          // exactly as the read path does.
          ++overruns_;
          if (snd_pcm_prepare(capture_) < 0) {
            fill_silence(destination + offset, remaining);
            return fail(error, "capture overrun and the device would not prepare");
          }
          std::string restart_error;
          if (!start_stream(capture_, &restart_error)) {
            log().write(
                LogLevel::warn,
                "cannot restart capture after an overrun: " + restart_error);
          }
          fill_silence(destination + offset, remaining);
          if (error != nullptr) {
            *error = "capture overrun (recovered while waiting)";
          }
          return true;
        }
        fill_silence(destination + offset, remaining);
        return fail(error,
                    "capture wait failed: " + std::string(snd_strerror(ready)));
      }
      if (ready == 0) {
        // Half a second with nothing captured at all. The caller still gets its
        // whole period — silence — because a short read would push the arithmetic
        // of starvation into every caller, but it gets a reason as well: on this
        // appliance a capture device that produces nothing is usually unlocked
        // PTP or a daemon bound to `lo`, and both are silent failures elsewhere.
        fill_silence(destination + offset, remaining);
        stalled = true;
        break;
      }

      const snd_pcm_sframes_t got =
          snd_pcm_readi(capture_, destination + offset, remaining);
      if (got == -EPIPE) {
        // Overrun: the device buffer was not drained in time, so those frames are
        // gone. Count it, recover, and return silence for the rest of the period
        // rather than failing the read: failing made the caller sleep, which
        // turned one overrun into a cascade.
        ++overruns_;
        if (snd_pcm_prepare(capture_) < 0) {
          fill_silence(destination + offset, remaining);
          return fail(error, "capture overrun and the device would not prepare");
        }
        // prepare() leaves the stream PREPARED, which stops the driver's tick for
        // this direction: trigger it again or capture never resumes.
        std::string restart_error;
        if (!start_stream(capture_, &restart_error)) {
          log().write(LogLevel::warn,
                      "cannot restart capture after an overrun: " + restart_error);
        }
        fill_silence(destination + offset, remaining);
        if (error != nullptr) {
          *error = "capture overrun (recovered)";
        }
        return true;
      }
      if (got < 0) {
        if (snd_pcm_recover(capture_, static_cast<int>(got), 1) < 0) {
          fill_silence(destination + offset, remaining);
          return fail(error,
                      "capture read failed: " + std::string(snd_strerror(got)));
        }
        continue;
      }
      if (got == 0) {
        continue;
      }
      offset += static_cast<size_t>(got) * frame_bytes;
      remaining -= static_cast<unsigned>(got);
      last_frames_at_ = mono_seconds();
    }
  }

  if (stalled) {
    recover_if_stalled();
    return fail(error, "RAVENNA capture produced nothing for " +
                           std::to_string(k_wait_timeout_ms) +
                           " ms: is the PTP clock locked, and is the daemon's "
                           "interface_name off \"lo\"?");
  }
  return true;
}

bool RavennaBackend::write(const uint8_t* source, unsigned frames,
                           std::string* error) {
  if (!open_ || playback_ == nullptr) {
    return fail(error, "the RAVENNA playback device is not open");
  }
  if (source == nullptr) {
    return fail(error, "no audio to write");
  }

  // Shared for the whole call: the two directions run on their own threads, and
  // only a stall recovery needs them both quiet.
  std::shared_lock<std::shared_mutex> lock(path_mutex_);

  const size_t frame_bytes = format_.frames_to_bytes(1);
  unsigned remaining = frames;
  size_t offset = 0;
  while (remaining > 0) {
    const int ready = snd_pcm_wait(playback_, k_wait_timeout_ms);
    if (ready < 0) {
      if (ready == -EPIPE) {
        // Same as the capture side: the wait reports the underrun first, and
        // without recovering here the device never ticks again.
        ++underruns_;
        if (snd_pcm_prepare(playback_) < 0) {
          return fail(error, "playback underrun and the device would not prepare");
        }
        std::string restart_error;
        if (!start_stream(playback_, &restart_error)) {
          log().write(
              LogLevel::warn,
              "cannot restart playback after an underrun: " + restart_error);
        }
        if (error != nullptr) {
          *error = "playback underrun (recovered while waiting)";
        }
        return true;
      }
      return fail(error,
                  "playback wait failed: " + std::string(snd_strerror(ready)));
    }
    if (ready == 0) {
      // Nothing was consumed: the device is not ticking, so there is no point
      // counting an underrun — no audio was lost, the device simply never asked
      // for it. The caller is told, because audio that reaches the device and
      // then goes nowhere is invisible from anywhere else.
      return fail(error, "RAVENNA playback consumed nothing for " +
                             std::to_string(k_wait_timeout_ms) +
                             " ms: is the PTP clock locked, and is the daemon's "
                             "interface_name off \"lo\"?");
    }

    const snd_pcm_sframes_t written =
        snd_pcm_writei(playback_, source + offset, remaining);
    if (written == -EPIPE) {
      // Underrun: the ring ran dry. Count it, recover, and drop the rest of this
      // period rather than failing the write — see the matching note in read().
      // prepare() stops the driver's tick for this direction, so the ring is
      // re-primed and triggered again by start_stream().
      ++underruns_;
      if (snd_pcm_prepare(playback_) < 0) {
        return fail(error, "playback underrun and the device would not prepare");
      }
      std::string restart_error;
      if (!start_stream(playback_, &restart_error)) {
        log().write(LogLevel::warn,
                    "cannot restart playback after an underrun: " + restart_error);
      }
      if (error != nullptr) {
        *error = "playback underrun (recovered)";
      }
      return true;
    }
    if (written < 0) {
      if (snd_pcm_recover(playback_, static_cast<int>(written), 1) < 0) {
        return fail(error,
                    "playback write failed: " + std::string(snd_strerror(written)));
      }
      continue;
    }
    if (written == 0) {
      continue;
    }
    offset += static_cast<size_t>(written) * frame_bytes;
    remaining -= static_cast<unsigned>(written);
  }
  return true;
}

std::string RavennaBackend::kind() const {
  return "ravenna";
}

std::string RavennaBackend::detail() const {
  std::string out = config_.device + " " + std::to_string(format_.channels) +
                    "ch @" + std::to_string(format_.sample_rate) + "Hz " +
                    config_.format;
  if (open_ && capture_ != nullptr && playback_ != nullptr) {
    // Report the substream state. A PREPARED stream means the driver's audio
    // engine never started, which is silent — no capture, nothing transmitted —
    // and reports no error, so the state name is the only evidence there is.
    out += std::string(", capture ") + snd_pcm_state_name(snd_pcm_state(capture_)) +
           ", playback " + snd_pcm_state_name(snd_pcm_state(playback_));
  }
  return out;
}

const AudioFormat& RavennaBackend::format() const {
  return format_;
}

const std::string& RavennaBackend::device() const {
  return config_.device;
}

unsigned RavennaBackend::overruns() const {
  return overruns_.load();
}

unsigned RavennaBackend::underruns() const {
  return underruns_.load();
}

}  // namespace aes67_srt::audio
