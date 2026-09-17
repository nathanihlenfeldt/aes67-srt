#include "app.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <memory>
#include <thread>

#include "aes67/daemon_client.hpp"
#include "commissioning.hpp"
#include "engine.hpp"
#include "http/api_server.hpp"
#include "log.hpp"
#include "version.hpp"

namespace aes67_srt {
namespace {

std::atomic<bool> stop_requested{false};

void handle_signal(int) {
  stop_requested.store(true);
}

}  // namespace

void App::configure(const Config& config) {
  config_ = config;
}

void App::set_config_path(const std::string& path) {
  config_path_ = path;
}

const Config& App::config() const {
  return config_;
}

bool App::fake() const {
  return fake_;
}

void App::apply_fake() {
  // `-f` means "touch nothing real", and there are exactly three ways out of
  // this process: the audio device, the daemon, and the link. All three are
  // replaced here, so a production configuration can be run on a laptop without
  // being edited — which is the whole point of having the switch.
  config_.audio.backend = "null";
  config_.daemon.fake = true;
  config_.link.mode = "loopback";
  // With a loopback there is no peer to reach, and leaving a stale one in place
  // would put a hostname in the log that nothing is connecting to.
  config_.link.peer.clear();
}

void App::set_fake(bool fake) {
  fake_ = fake;
  if (fake_) {
    apply_fake();
  }
}

void App::set_subscription(Subscription subscription,
                           const std::string& subscribe_to) {
  subscription_ = subscription;
  subscribe_to_ = subscribe_to;
}

void App::describe() const {
  log().write(LogLevel::info, std::string("aes67-srt ") + version_string() + " (" +
                                  build_info() + ")");
  log().write(LogLevel::info,
              "audio: " + config_.audio.backend + " " + config_.audio.device + " " +
                  std::to_string(config_.audio.channels) + "ch " +
                  std::to_string(config_.audio.sample_rate) + " Hz " +
                  config_.audio.format + ", " +
                  std::to_string(config_.audio.period_frames) + " frame periods");
  log().write(
      LogLevel::info,
      "link: role=" + config_.link.role + " mode=" + config_.link.mode +
          " peer=" + (config_.link.peer.empty() ? "(none)" : config_.link.peer) +
          " latency=" + std::to_string(config_.link.latency_ms) +
          " ms alarm=" + std::to_string(config_.link.alarm_delay_ms) + " ms");
  log().write(LogLevel::info, "blocks: " + std::to_string(config_.blocks.size()) +
                                  " x 8 channels, egress delay " +
                                  std::to_string(config_.egress.delay_ms) + " ms");
  if (fake_) {
    log().write(LogLevel::warn,
                "fake mode: null audio backend, fake daemon, loopback transport");
  }

  // What the path now does, module by module: the clock holds the playout delay
  // and resamples at the ratio it decides, the delay line adds the A/V offset and
  // the test signal, and the control surface serves the status page the operator
  // reads. A build that carries audio should say where it stops, and it no longer
  // stops anywhere before the device.
  log().write(
      LogLevel::info,
      "audio path: device -> blocks -> frame -> link -> clock -> delay -> device, "
      "the clock holding the playout delay and resampling at the ratio it decides, "
      "the delay line adding the A/V offset and the test signal (ticket 12); the "
      "control surface serves the status page and the REST API (ticket 13)");
}

void App::request_stop() {
  stop_requested.store(true);
}

int App::run() {
  std::signal(SIGINT, handle_signal);
  std::signal(SIGTERM, handle_signal);
  // SIGHUP is `systemctl reload`: a clean stop, after which the unit's
  // Restart=always brings the appliance back with the file on disk. Reloading in
  // place would have to re-open the link, rebuild the device and republish on the
  // daemon -- which is a restart, and pretending otherwise is how a half-applied
  // configuration happens.
  std::signal(SIGHUP, handle_signal);
  // Reset for a second run in the same process (which is what a test does): the
  // flag is a file-scope global because a signal handler cannot be given one.
  stop_requested.store(false);

  // Applied again here, not only in set_fake(): the public calls can arrive in
  // either order, and what matters is that the configuration the engine is built
  // from is the one described and the one that runs.
  if (fake_) {
    apply_fake();
  }
  describe();

  std::string error;

  // The daemon seam, before the engine starts: publish what this appliance
  // transmits, and — when commissioning — subscribe to ourselves. Doing it first
  // means a daemon that refuses is reported before audio is running, which is
  // the difference between a sentence and a hunt.
  //
  // A failure here does *not* stop the appliance, because the SRT link does not
  // depend on the AES67 wiring: it is the daemon that feeds the device, so an
  // unwired daemon produces silence rather than a broken link, and refusing to
  // start would take down the half that works. Whether "no AES67 wiring" should
  // be a refusal is the preflight's question (ticket 13), not this one's.
  //
  // Unless the operator asked for the loopback specifically, in which case the
  // run *is* the test and a failed test is a failed run.
  std::unique_ptr<daemon::DaemonClient> daemon =
      daemon::DaemonClient::create(config_.daemon);
  CommissioningResult commissioning;
  if (!commission(daemon.get(), config_, subscription_, subscribe_to_,
                  &commissioning, &error)) {
    log().write(LogLevel::error, error);
    if (subscription_ != Subscription::none) {
      return static_cast<int>(ExitCode::runtime_error);
    }
  }

  Engine engine;
  if (!engine.prepare(config_, &error)) {
    log().write(LogLevel::error, error);
    return static_cast<int>(ExitCode::runtime_error);
  }

  // The control surface (ticket 13), on its own thread for as long as the engine
  // runs. A port that will not bind is a warning rather than a failure: audio is
  // the point, and a second instance's collision should not take down a working
  // link just because it cannot also have the status page.
  ApiServer control(&config_, &engine, daemon.get(), std::string(), config_path_);
  if (!control.start(&error)) {
    log().write(LogLevel::warn,
                error + " (continuing without the control surface)");
  }

  // The engine owns both threads and paces itself by the device, so nothing here
  // has to drive it: this thread waits for a signal and then asks it to stop.
  // Run on a thread of its own so that a stop request is answered promptly
  // rather than after the current read or receive returns.
  int result = 0;
  std::thread running([&engine, &result] { result = engine.run(); });
  while (!stop_requested.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  log().write(LogLevel::info, "stopping");
  engine.stop();
  running.join();
  control.stop();

  return result == 0 ? static_cast<int>(ExitCode::ok)
                     : static_cast<int>(ExitCode::runtime_error);
}

}  // namespace aes67_srt
