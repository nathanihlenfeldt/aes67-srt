#include "audio/null_backend.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <thread>

namespace aes67_srt::audio {
namespace {

bool fail(std::string* error, const std::string& message) {
  if (error != nullptr) {
    *error = message;
  }
  return false;
}

}  // namespace

void NullBackend::pace(std::chrono::steady_clock::time_point* next) const {
  // A device hands over a period when the period is over, and not before. The
  // engine is built on that: it is paced by the device rather than by a timer of
  // its own, because the whole design depends on there being as few timebases to
  // reconcile as possible (ADR 0001).
  //
  // Deliberately not under the mutex: sleeping while holding it would make each
  // direction wait out the other's period, halving both.
  const auto now = std::chrono::steady_clock::now();
  if (*next > now) {
    std::this_thread::sleep_until(*next);
    *next += period_;
    return;
  }
  // Behind, or the first call after opening: reschedule from now rather than
  // trying to make up a debt, which would arrive as a burst of periods.
  *next = now + period_;
}

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

  // The rate this device hands audio over at, and the two directions' schedules.
  period_ = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
      std::chrono::duration<double>(static_cast<double>(format_.period_frames) /
                                    static_cast<double>(format_.sample_rate)));
  next_read_tick_ = std::chrono::steady_clock::now();
  next_write_tick_ = next_read_tick_;
  open_ = true;
  return true;
}

void NullBackend::close() {
  std::lock_guard<std::mutex> lock(mutex_);
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
  // Paced before the lock, because a device blocks before it takes the audio.
  pace(&next_write_tick_);

  const size_t bytes = format_.frames_to_bytes(frames);
  std::lock_guard<std::mutex> lock(mutex_);
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
  // Paced before the lock: a device blocks until it has a period, and only then
  // hands it over.
  pace(&next_read_tick_);

  const size_t bytes = format_.frames_to_bytes(frames);
  std::lock_guard<std::mutex> lock(mutex_);

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
  return overruns_.load();
}

unsigned NullBackend::underruns() const {
  return underruns_.load();
}

size_t NullBackend::pending_bytes() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return pending_;
}

}  // namespace aes67_srt::audio
