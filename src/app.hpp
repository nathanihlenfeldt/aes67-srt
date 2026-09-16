#pragma once

#include <string>

#include "config.hpp"

namespace aes67_srt {

/** Exit codes, so systemd and the check script can tell failures apart. */
enum class ExitCode { ok = 0, usage = 2, config_error = 3, runtime_error = 4 };

/**
 * The supervisor.
 *
 * This skeleton owns the configuration and the run/stop lifecycle that the
 * audio, transport and control modules will hang off.  It deliberately carries
 * no audio: the frame skeleton is ticket 05, and the audio and transport
 * modules are tickets 07 and 08.
 */
class App {
 public:
  App() = default;

  /** Keep a configuration that has already been validated. */
  void configure(const Config& config);

  const Config& config() const;

  /** True when the backend and daemon are faked, so nothing touches hardware. */
  bool fake() const;
  void set_fake(bool fake);

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

 private:
  Config config_;
  bool fake_ = false;
};

}  // namespace aes67_srt
