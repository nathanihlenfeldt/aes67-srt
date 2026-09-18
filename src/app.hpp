#pragma once

#include <memory>
#include <mutex>
#include <string>

#include "commissioning.hpp"
#include "config.hpp"
#include "exit_code.hpp"
#include "service.hpp"

namespace aes67_srt {

/**
 * The supervisor.
 *
 * Owns the configuration, the run/stop lifecycle, and the whole audio path: it
 * builds the device and the link, runs the two directions with the clock and the
 * A/V delay line in the receive path, and serves the control surface's status page
 * while it runs.
 *
 * The log is still one way to see it. It is no longer the only one.
 */
class App {
 public:
  App() = default;

  /** Keep a configuration that has already been validated. */
  void configure(const Config& config);

  /**
   * Where the configuration came from, so a change from the control surface can be
   * written back. Empty means "do not persist": a change is then refused rather
   * than applied and lost on the next start.
   */
  void set_config_path(const std::string& path);

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
   * Subscribe the appliance's sinks, and to what.
   *
   * `none` is the operating default: an appliance publishes its sources and the
   * site wires the receive side. The other two are commissioning choices — the
   * first block subscribed to a discovered announcement by name, or every block
   * subscribed to this appliance's own source.
   */
  void set_subscription(Subscription subscription, const std::string& subscribe_to);

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
  /** Guards `config_` across the control surface, the engine's restart and this. */
  std::mutex config_mutex_;
  /** Owns the engine's lifecycle so the control surface can start and stop it. */
  std::unique_ptr<EngineService> service_;
  std::string config_path_;
  bool fake_ = false;
  Subscription subscription_ = Subscription::none;
  std::string subscribe_to_;
};

}  // namespace aes67_srt
