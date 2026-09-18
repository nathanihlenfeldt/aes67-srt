// The macOS endpoint: SRT to CoreAudio, the family's second product (issue #19).
//
// It deliberately shares almost nothing with `main.cpp` but the core: no
// commissioning, no daemon, no PTP. The appliance's supervisor talks to
// `aes67-daemon` because its audio comes from an AES67 fabric; here the audio
// comes from a CoreAudio device and the other end is this project's appliance over
// SRT. ADR 0004 is the constraint this file exists to honour.

#include <atomic>
#include <chrono>
#include <csignal>
#include <iostream>
#include <string>
#include <thread>

#include "config.hpp"
#include "engine.hpp"
#include "exit_code.hpp"
#include "http/api_server.hpp"
#include "log.hpp"
#include "service.hpp"
#include "version.hpp"

namespace {

std::atomic<bool> stop_requested{false};

void handle_signal(int) {
  stop_requested.store(true);
}

void print_usage(const char* program) {
  std::cout
      << "usage: " << program
      << " [-c <config>] [-f] [--validate] [-a <addr>] [-p <port>] [-v] [-h]\n"
      << "\n"
      << "  -c <config>   configuration file (default /etc/aes67-srt-mac.conf)\n"
      << "  -f            null audio, loopback link: develop with no device\n"
      << "  --validate    check the configuration and exit; nothing runs\n"
      << "  -a <addr>     override http_addr\n"
      << "  -p <port>     override http_port\n"
      << "  -v            print version and exit\n"
      << "  -h            this message\n";
}

}  // namespace

int main(int argc, char** argv) {
  std::string config_path = "/etc/aes67-srt-mac.conf";
  std::string http_addr_override;
  std::string http_port_override;
  bool validate_only = false;
  bool fake = false;

  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    if (argument == "-c" && index + 1 < argc) {
      config_path = argv[++index];
    } else if (argument == "-f") {
      fake = true;
    } else if (argument == "--validate") {
      validate_only = true;
    } else if (argument == "-a" && index + 1 < argc) {
      http_addr_override = argv[++index];
    } else if (argument == "-p" && index + 1 < argc) {
      http_port_override = argv[++index];
    } else if (argument == "-v" || argument == "--version") {
      std::cout << "aes67-srt-mac " << aes67_srt::version_string() << " ("
                << aes67_srt::build_info() << ")" << std::endl;
      return static_cast<int>(aes67_srt::ExitCode::ok);
    } else if (argument == "-h" || argument == "--help") {
      print_usage(argv[0]);
      return static_cast<int>(aes67_srt::ExitCode::ok);
    } else {
      std::cerr << "unknown argument: " << argument << std::endl;
      print_usage(argv[0]);
      return static_cast<int>(aes67_srt::ExitCode::usage);
    }
  }

  aes67_srt::Config config;
  std::string reason;
  if (!aes67_srt::load_config(config_path, &config, &reason)) {
    std::cerr << reason << std::endl;
    return static_cast<int>(aes67_srt::ExitCode::config_error);
  }
  if (!http_addr_override.empty()) {
    config.http_addr = http_addr_override;
  }
  if (!http_port_override.empty()) {
    config.http_port = std::stoi(http_port_override);
  }
  if (fake) {
    // Development with no device: the same escape hatch the appliance has, kept
    // to the two things this product can fake — its device and its peer.
    config.audio.backend = "null";
    config.link.mode = "loopback";
    config.link.peer.clear();
  }
  if (!config.validate(&reason)) {
    std::cerr << "command line: " << reason << std::endl;
    return static_cast<int>(aes67_srt::ExitCode::config_error);
  }

  if (validate_only) {
    std::cout << "ok: " << config_path << " (" << config.blocks.size()
              << " blocks, " << config.audio.channels << " channels, backend "
              << config.audio.backend << ", device " << config.audio.device << ")"
              << std::endl;
    return static_cast<int>(aes67_srt::ExitCode::ok);
  }

  aes67_srt::log().write(aes67_srt::LogLevel::info,
                         std::string("aes67-srt-mac ") +
                             aes67_srt::version_string() + " (" +
                             aes67_srt::build_info() + ")");
  aes67_srt::log().write(aes67_srt::LogLevel::info,
                         "audio: " + config.audio.backend + " " +
                             config.audio.device + " " +
                             std::to_string(config.audio.channels) + "ch " +
                             std::to_string(config.audio.sample_rate) + " Hz");
  if (fake) {
    aes67_srt::log().write(aes67_srt::LogLevel::warn,
                           "fake mode: null audio, loopback link");
  }

  // The engine is owned by a service so the control surface can stop and start
  // it while the process stays up (issue #33).
  std::mutex config_mutex;
  aes67_srt::EngineService service(&config, &config_mutex, config_path);

  // No daemon client: this product carries no AES67 and no PTP (ADR 0004), so the
  // control surface starts with a null daemon and its preflight drops the daemon
  // and PTP checks rather than reporting failures nobody here can fix.
  aes67_srt::ApiServer control(&config, &service, nullptr, std::string(),
                               config_path);
  if (!control.start(&reason)) {
    aes67_srt::log().write(aes67_srt::LogLevel::warn,
                           reason + " (continuing without the control surface)");
  }

  std::signal(SIGINT, handle_signal);
  std::signal(SIGTERM, handle_signal);
  std::signal(SIGHUP, handle_signal);
  stop_requested.store(false);

  if (!service.start(&reason)) {
    aes67_srt::log().write(aes67_srt::LogLevel::error, reason);
    control.stop();
    return static_cast<int>(aes67_srt::ExitCode::runtime_error);
  }
  while (!stop_requested.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  aes67_srt::log().write(aes67_srt::LogLevel::info, "stopping");
  service.stop();
  control.stop();

  return static_cast<int>(aes67_srt::ExitCode::ok);
}
