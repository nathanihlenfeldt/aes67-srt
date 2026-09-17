#pragma once

#include <string>

#include "config.hpp"

namespace aes67_srt {

/** Exit codes, so systemd and the check script can tell failures apart. */
enum class ExitCode { ok = 0, usage = 2, config_error = 3, runtime_error = 4 };

/**
 * The supervisor.
 *
 * Owns the configuration, the run/stop lifecycle, and — since the engine landed —
 * the audio path itself: it builds the device and the link, runs the two
 * directions, and stops them when the process is asked to.
 *
 * What it does *not* own yet: the clock and delay stages, which are later
 * tickets, and the control surface. So audio passes through unaltered, and the
 * only way to see or steer it is the log.
 */
class App {
 public:
  App() = default;

  /** Keep a configuration that has already been validated. */
  void configure(const Config& config);

  const Config& config() const;

  /** True when the backend and daemon are faked, so nothing touches hardware. */
  bool fake() const;

  /**
   * Turn fake mode on or off.
   *
   * Setting it *applies* it to the configuration immediately — a null audio
   * device, the simulated daemon, and the in-process loopback link — because
   * `-f` means one thing: nothing this process does touches a device, a network
   * or somebody else's daemon. `config()` then reports what will actually run,
   * rather than what the file said before `-f` was processed.
   */
  void set_fake(bool fake);

  /**
   * Ask the daemon to subscribe each block's sink to this appliance's own
   * source: the commissioning loopback, transmitting to ourselves.
   *
   * Off by default, because it is a *test* rather than an operating mode — a
   * site wires its sinks to the far end, not to the box in front of it.
   */
  void set_commissioning_loopback(bool enabled);

  /**
   * Log what this build is and what it is not.
   *
   * Printed on every start: an appliance in someone else's building is
   * diagnosed from its log, and "which modules does this binary actually
   * contain" should never be a question.
   */
  void describe() const;

  /** Run until interrupted. Returns an ExitCode. */
  int run();

  /**
   * Ask run() to finish, as a signal would.
   *
   * The programmatic half of the same lifecycle the signal handler drives, and
   * deliberately not a test-only hook: anything that can stop the appliance —
   * a signal, a systemd stop, a future control-surface request — should go
   * through one flag rather than each inventing its own way. Safe to call from
   * another thread.
   */
  void request_stop();

 private:
  /** Whatever `-f` implies, applied to the configuration. Idempotent. */
  void apply_fake();

  Config config_;
  bool fake_ = false;
  bool commissioning_loopback_ = false;
};

}  // namespace aes67_srt
