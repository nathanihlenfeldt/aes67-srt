#include "app.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <thread>

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

void App::set_fake(bool fake) {
  fake_ = fake;
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

  // Say plainly what is missing, so a build that carries no audio is never
  // mistaken for one that has lost its audio.
  log().write(LogLevel::info,
              "not built yet: audio (ticket 08), transport (ticket 07), "
              "clock (ticket 11), control surface (tickets 13-14)");
}

int App::run() {
  std::signal(SIGINT, handle_signal);
  std::signal(SIGTERM, handle_signal);

  describe();
  log().write(LogLevel::info,
              "running (skeleton: no audio path yet - Ctrl-C to stop)");

  // Nothing to serve and nothing to move yet, so this waits for a signal rather
  // than exiting: a daemon that returns immediately would be restarted in a
  // loop by systemd, which looks exactly like a crash.
  while (!stop_requested.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }

  log().write(LogLevel::info, "stopping");
  return static_cast<int>(ExitCode::ok);
}

}  // namespace aes67_srt
