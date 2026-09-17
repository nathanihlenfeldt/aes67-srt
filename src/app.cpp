#include "app.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <thread>

#include "engine.hpp"
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

  // What is missing from the *path*, now that the modules themselves exist:
  // there is no clock stage and no delay stage, so audio passes through
  // unaltered, and no control surface can see or steer any of it yet. A build
  // that carries audio should say where it stops, for the same reason the old
  // line said what it did not carry at all.
  log().write(LogLevel::info,
              "audio path: device -> blocks -> frame -> link and back, passing "
              "through unaltered: no clock stage (ticket 11), no delay stage "
              "(ticket 12), no control surface (tickets 13-14)");
}

void App::request_stop() {
  stop_requested.store(true);
}

int App::run() {
  std::signal(SIGINT, handle_signal);
  std::signal(SIGTERM, handle_signal);
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

  Engine engine;
  std::string error;
  if (!engine.prepare(config_, &error)) {
    log().write(LogLevel::error, error);
    return static_cast<int>(ExitCode::runtime_error);
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

  return result == 0 ? static_cast<int>(ExitCode::ok)
                     : static_cast<int>(ExitCode::runtime_error);
}

}  // namespace aes67_srt
