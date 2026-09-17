#include "app.hpp"

#include <chrono>
#include <string>
#include <thread>

#include "config.hpp"
#include "test_framework.hpp"

namespace {

using aes67_srt::App;
using aes67_srt::Config;
using aes67_srt::ExitCode;

/** The production shape: every one of the three ways out of the process is real. */
Config production_config() {
  Config config;
  config.audio.backend = "ravenna";
  config.audio.channels = 8;
  config.link.blocks = 1;
  config.link.mode = "caller";
  config.link.peer = "remote.example.com:9000";
  config.daemon.fake = false;
  config.fill_default_blocks();
  return config;
}

}  // namespace

TEST_CASE(app_fake_mode_replaces_everything_that_leaves_this_process) {
  App app;
  app.configure(production_config());
  CHECK(!app.fake());
  CHECK_EQ(app.config().audio.backend, std::string("ravenna"));

  app.set_fake(true);
  CHECK(app.fake());

  // Three ways out of this process, all three replaced: the audio device, the
  // daemon, and the link. Anything left real would be a way for a laptop run to
  // reach a site — which is precisely what `-f` promises not to do.
  CHECK_EQ(app.config().audio.backend, std::string("null"));
  CHECK_EQ(app.config().daemon.fake, true);
  CHECK_EQ(app.config().link.mode, std::string("loopback"));
  // And no stale peer left in the log for a link that is not connecting to one.
  CHECK(app.config().link.peer.empty());
}

TEST_CASE(app_leaves_a_configuration_that_did_not_ask_for_fake_mode_alone) {
  App app;
  app.configure(production_config());
  // Only the switch changes a configuration. A device, a daemon and a peer that
  // were configured deliberately must survive to the engine untouched.
  CHECK_EQ(app.config().audio.backend, std::string("ravenna"));
  CHECK_EQ(app.config().link.mode, std::string("caller"));
  CHECK_EQ(app.config().link.peer, std::string("remote.example.com:9000"));
  CHECK_EQ(app.config().daemon.fake, false);
}

TEST_CASE(app_runs_the_whole_audio_path_in_fake_mode_and_stops_cleanly) {
  // The appliance's actual lifecycle, with nothing real anywhere: a null device,
  // the in-process loopback, the engine's two threads, and a clean stop. This is
  // what `build/aes67-srt -c config/aes67-srt.dev.conf -f` does on a laptop, and
  // until this test existed nothing had ever run it.
  App app;
  app.configure(production_config());
  app.set_fake(true);

  int result = -1;
  std::thread running([&app, &result] { result = app.run(); });

  // Long enough for the engine to open the device and the link and turn over
  // several hundred periods of audio before being asked to stop.
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  app.request_stop();
  running.join();

  CHECK_EQ(result, static_cast<int>(ExitCode::ok));
}
