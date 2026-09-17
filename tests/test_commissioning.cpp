#include "commissioning.hpp"

#include <memory>
#include <string>

#include "aes67/daemon_client.hpp"
#include "aes67/fake_daemon_client.hpp"
#include "config.hpp"
#include "test_framework.hpp"

namespace {

using aes67_srt::commission;
using aes67_srt::CommissioningResult;
using aes67_srt::Config;
using aes67_srt::DaemonConfig;
using aes67_srt::Subscription;
using aes67_srt::daemon::DaemonClient;
using aes67_srt::daemon::json;

/** A configuration with `blocks` 8-channel blocks, on the fake daemon. */
Config commissioning_config(size_t blocks) {
  Config config;
  config.audio.backend = "null";
  config.audio.channels = static_cast<int>(blocks * 8);
  config.link.blocks = static_cast<int>(blocks);
  config.daemon.address = "127.0.0.1";
  config.daemon.port = 8080;
  config.daemon.fake = true;
  config.fill_default_blocks();
  return config;
}

bool contains(const std::string& text, const std::string& needle) {
  return text.find(needle) != std::string::npos;
}

}  // namespace

TEST_CASE(commissioning_publishes_one_source_per_block_with_its_own_channels) {
  const Config config = commissioning_config(8);
  std::unique_ptr<DaemonClient> daemon = DaemonClient::create(config.daemon);
  CommissioningResult result;
  std::string error;

  CHECK(commission(daemon.get(), config, Subscription::none, std::string(), &result,
                   &error));
  CHECK_EQ(result.sources_published, static_cast<size_t>(8));
  // Nothing was asked for the other direction, so nothing was subscribed.
  CHECK_EQ(result.sinks_subscribed, static_cast<size_t>(0));

  json sources;
  CHECK(daemon->get_sources(&sources, &error));
  const json& list = sources.at("sources");
  CHECK_EQ(list.size(), static_cast<size_t>(8));

  // Stream id i is block i, and each source carries that block's own device
  // channels — the same map the engine packs frames from, so the AES67 side and
  // the wire side cannot disagree about which channels are which.
  for (size_t index = 0; index < 8; ++index) {
    const json& source = list.at(index);
    CHECK_EQ(source.at("id").get<int>(), static_cast<int>(index));
    CHECK_EQ(source.at("map"), json(config.blocks[index].channels));
    CHECK_EQ(source.at("codec").get<std::string>(), std::string("L24"));
    CHECK_EQ(source.at("max_samples_per_packet").get<int>(), 48);
  }
}

TEST_CASE(commissioning_the_loopback_subscribes_each_sink_to_our_own_source) {
  const Config config = commissioning_config(8);
  std::unique_ptr<DaemonClient> daemon = DaemonClient::create(config.daemon);
  CommissioningResult result;
  std::string error;

  CHECK(commission(daemon.get(), config, Subscription::self, std::string(), &result,
                   &error));
  CHECK_EQ(result.sources_published, static_cast<size_t>(8));
  CHECK_EQ(result.sinks_subscribed, static_cast<size_t>(8));
  // The daemon's own answer is the outcome that matters: a sink reporting
  // packets arriving means the source was published, discovered, subscribed and
  // is being carried — which is the whole of what commissioning is asking.
  CHECK_EQ(result.sinks_receiving, static_cast<size_t>(8));
  CHECK_EQ(result.ptp_status, std::string("locked"));

  // And the sinks really are the daemon's, with the SDP the daemon itself
  // reports for our source rather than one invented here.
  json sinks;
  CHECK(daemon->get_sinks(&sinks, &error));
  const json& list = sinks.at("sinks");
  CHECK_EQ(list.size(), static_cast<size_t>(8));
  for (size_t index = 0; index < 8; ++index) {
    const json& sink = list.at(index);
    const std::string sdp = sink.at("sdp").get<std::string>();
    CHECK(contains(sdp, "L24/48000/8"));
    // The daemon skips no clock-domain check on our behalf, and a stream from
    // another grandmaster would be refused rather than played wrongly.
    CHECK_EQ(sink.at("ignore_refclk_gmid").get<bool>(), false);
    CHECK_EQ(sink.at("map"), json(config.blocks[index].channels));
  }
}

TEST_CASE(commissioning_reports_an_unlocked_slave_without_refusing_to_wire) {
  const Config config = commissioning_config(2);
  std::unique_ptr<DaemonClient> daemon = DaemonClient::create(config.daemon);
  auto* fake = dynamic_cast<aes67_srt::daemon::FakeDaemonClient*>(daemon.get());
  CHECK(fake != nullptr);
  fake->set_ptp_status("unlocked");

  CommissioningResult result;
  std::string error;
  // Unlocked PTP is the commonest reason an appliance produces no audio while
  // looking healthy, so it must be *reported* — but the streams are still
  // published, because the lock often arrives a few seconds later and refusing
  // to wire up would then need a restart to fix.
  CHECK(commission(daemon.get(), config, Subscription::none, std::string(), &result,
                   &error));
  CHECK_EQ(result.ptp_status, std::string("unlocked"));
  CHECK_EQ(result.sources_published, static_cast<size_t>(2));
}

TEST_CASE(commissioning_subscribes_block_zero_to_a_named_announcement) {
  // The path that matters on a real bench: there is a genuine eight-channel AES67
  // sender on the network, and the appliance's job on the receive side is to
  // subscribe to it. The SDP has to come from the daemon's *discovery* rather
  // than from anything we invent, so what this asserts is the round trip:
  // browse, pick by name, and put the daemon's own SDP into the sink.
  const Config config = commissioning_config(8);
  std::unique_ptr<DaemonClient> daemon = DaemonClient::create(config.daemon);
  CommissioningResult result;
  std::string error;

  CHECK(commission(daemon.get(), config, Subscription::discovered, "AES67-TX-1",
                   &result, &error));
  CHECK_EQ(result.sources_published, static_cast<size_t>(8));
  // One sink, not eight: the sender has eight channels, and eight sinks on one
  // eight-channel stream would be the same audio eight times over.
  CHECK_EQ(result.sinks_subscribed, static_cast<size_t>(1));
  CHECK_EQ(result.sinks_receiving, static_cast<size_t>(1));

  json sinks;
  CHECK(daemon->get_sinks(&sinks, &error));
  CHECK_EQ(sinks.at("sinks").size(), static_cast<size_t>(1));
  const json& sink = sinks.at("sinks").at(0);
  CHECK_EQ(sink.at("id").get<int>(), 0);
  CHECK(contains(sink.at("sdp").get<std::string>(), "AES67-TX-1"));
  CHECK(contains(sink.at("sdp").get<std::string>(), "L24/48000/8"));
  // Block 0's channels, so the eight channels of the announcement land there.
  CHECK_EQ(sink.at("map"), json(config.blocks[0].channels));
}

TEST_CASE(commissioning_refuses_a_name_that_was_not_discovered) {
  // A typo has to be answerable from the log. "No such source" without saying
  // what *was* found sends whoever typed it to a shell to curl the daemon.
  const Config config = commissioning_config(1);
  std::unique_ptr<DaemonClient> daemon = DaemonClient::create(config.daemon);
  CommissioningResult result;
  std::string error;

  CHECK(!commission(daemon.get(), config, Subscription::discovered, "AES67-TX-7",
                    &result, &error));
  CHECK(contains(error, "AES67-TX-7"));
  CHECK(contains(error, "AES67-TX-1"));
  CHECK(contains(error, "AES67-TX-2-qsys"));

  // And the sources were still published, because publishing comes first and is
  // not something a failed subscription should undo.
  CHECK_EQ(result.sources_published,
           static_cast<size_t>(0));  // result is written only at the end
  json sources;
  CHECK(daemon->get_sources(&sources, &error));
  CHECK_EQ(sources.at("sources").size(), static_cast<size_t>(1));
}

TEST_CASE(commissioning_refuses_when_there_is_no_daemon_to_talk_to) {
  // A real HTTP client pointed at a port nothing listens on: the failure has to
  // name the endpoint, because "the daemon did not answer" without saying which
  // address is a support call rather than a log line.
  Config config = commissioning_config(1);
  config.daemon.fake = false;
  config.daemon.port = 1;
  std::unique_ptr<DaemonClient> daemon = DaemonClient::create(config.daemon);

  CommissioningResult result;
  std::string error;
  CHECK(!commission(daemon.get(), config, Subscription::none, std::string(),
                    &result, &error));
  CHECK(contains(error, "127.0.0.1:1"));
  CHECK_EQ(result.sources_published, static_cast<size_t>(0));
}
