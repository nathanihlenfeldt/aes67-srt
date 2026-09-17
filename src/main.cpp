#include <cstring>
#include <iostream>
#include <string>

#include "app.hpp"
#include "config.hpp"
#include "log.hpp"
#include "version.hpp"

namespace {

void print_usage(const char* program) {
  std::cout
      << "usage: " << program
      << " [-c <config>] [-f] [--commission-loopback] [--validate] "
      << "[-a <addr>] [-p <port>] [-v] [-h]\n"
      << "\n"
      << "  -c <config>   configuration file (default /etc/aes67-srt.conf)\n"
      << "  -f            fake mode: null audio, fake daemon, loopback transport\n"
      << "  --commission  publish this appliance's sources on the daemon; asks\n"
      << "                for nothing back, and is what a site runs\n"
      << "  --subscribe <name|self>\n"
      << "                publish the sources and subscribe block 0 to the\n"
      << "                discovery announcement <name>, or to \"self\" for this\n"
      << "                appliance's own source (every block). A commissioning\n"
      << "                run, so it fails the process if the daemon refuses\n"
      << "  --validate    check the configuration and exit; nothing runs\n"
      << "  -a <addr>     override http_addr\n"
      << "  -p <port>     override http_port\n"
      << "  -v            print version and exit\n"
      << "  -h            this message\n";
}

}  // namespace

int main(int argc, char** argv) {
  std::string config_path = "/etc/aes67-srt.conf";
  std::string http_addr_override;
  std::string http_port_override;
  bool validate_only = false;
  bool fake = false;
  std::string subscribe_to;

  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    if (argument == "-c" && index + 1 < argc) {
      config_path = argv[++index];
    } else if (argument == "-f") {
      fake = true;
    } else if (argument == "--subscribe" && index + 1 < argc) {
      subscribe_to = argv[++index];
    } else if (argument == "--commission") {
      // Publishing is what the appliance does anyway; the flag exists so a script
      // can say "this will write to the daemon" out loud.
    } else if (argument == "--validate") {
      validate_only = true;
    } else if (argument == "-a" && index + 1 < argc) {
      http_addr_override = argv[++index];
    } else if (argument == "-p" && index + 1 < argc) {
      http_port_override = argv[++index];
    } else if (argument == "-v" || argument == "--version") {
      std::cout << "aes67-srt " << aes67_srt::version_string() << " ("
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

  // Command line overrides are validated like any other change: the point of
  // validating before applying is that there is no path into the running
  // appliance that skips it.
  if (!http_addr_override.empty()) {
    config.http_addr = http_addr_override;
  }
  if (!http_port_override.empty()) {
    config.http_port = std::stoi(http_port_override);
  }
  if (!config.validate(&reason)) {
    std::cerr << "command line: " << reason << std::endl;
    return static_cast<int>(aes67_srt::ExitCode::config_error);
  }

  if (validate_only) {
    std::cout << "ok: " << config_path << " (" << config.blocks.size()
              << " blocks, " << config.audio.channels << " channels, role "
              << config.link.role << ")" << std::endl;
    return static_cast<int>(aes67_srt::ExitCode::ok);
  }

  aes67_srt::App app;
  app.configure(config);
  app.set_config_path(config_path);
  app.set_fake(fake);
  // "self" is the one name that is not an announcement: it means this appliance's
  // own source, i.e. the commissioning loopback the spec asks for.
  if (subscribe_to == "self") {
    app.set_subscription(aes67_srt::Subscription::self, std::string());
  } else if (!subscribe_to.empty()) {
    app.set_subscription(aes67_srt::Subscription::discovered, subscribe_to);
  }
  return app.run();
}
