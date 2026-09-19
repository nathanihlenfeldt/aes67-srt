#pragma once

#include <memory>
#include <string>

#include "audio/backend.hpp"
#include "config.hpp"

namespace aes67_srt::audio {

/**
 * The macOS endpoint's device, seen from the application (ADR 0007, spec 0002,
 * issue #39): the application side of the shared-memory block the HAL plug-in
 * writes and reads.
 *
 * It is deliberately the same shape as every other `AudioBackend` — the engine sees
 * bytes and a period and knows nothing about CoreAudio or shared memory — so
 * `wire`, `transport` and `engine` are untouched by the endpoint's device.
 *
 * The inversion ADR 0005 describes is here, in a new place. With the AudioUnit
 * backend *the device calls us*, through a render callback in this process. With a
 * HAL plug-in the device is in **another** process, so the meeting point is the two
 * shared rings. This backend paces its caller by **the clock, one period per call**
 * — on macOS a virtual device's clock is the system clock, so that tracks the
 * device exactly — and moves audio without ever waiting on the rings: a starved
 * read is padded with silence, a full write gives up the oldest, and both are
 * counted. Waiting on the ring instead is the trap that froze a working link
 * whenever nothing was reading the device.
 *
 * `s24_3le` ↔ float32 conversion is done here, on the engine's side of the ring, in
 * ordinary code (ADR 0005): the ring is float because the HAL is, and the wire is
 * bytes because AES67 is.
 */
class HalBackend : public AudioBackend {
 public:
  /**
   * `region_name` is the POSIX shared-memory name to open; empty means the
   * product's own name (`kHalRegionName`). It is a parameter so a test can use a
   * private region and not collide with a driver that is installed on the machine.
   */
  explicit HalBackend(const AudioConfig& config, std::string region_name = {});
  ~HalBackend() override;

  HalBackend(const HalBackend&) = delete;
  HalBackend& operator=(const HalBackend&) = delete;

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

/** True when this build contains the shared-memory HAL backend. */
bool hal_backend_available();

}  // namespace aes67_srt::audio