#include "aes67/daemon_client.hpp"
#include "aes67/fake_daemon_client.hpp"

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "audio/backend.hpp"  // the period the daemon's packet size must equal
#include "config.hpp"
#include "test_framework.hpp"

namespace {

using aes67_srt::BlockConfig;
using aes67_srt::Config;
using aes67_srt::DaemonConfig;
using aes67_srt::daemon::block_stream_name;
using aes67_srt::daemon::DaemonClient;
using aes67_srt::daemon::FakeDaemonClient;
using aes67_srt::daemon::json;
using aes67_srt::daemon::make_block_sink;
using aes67_srt::daemon::make_block_source;
using aes67_srt::daemon::PtpStatus;
using aes67_srt::daemon::SinkStatus;

DaemonConfig fake_config() {
  DaemonConfig config;
  config.fake = true;
  return config;
}

/** The accepted configuration: 64 channels as eight blocks of eight. */
Config default_config() {
  Config config;
  config.fill_default_blocks();
  return config;
}

bool contains(const std::string& text, const std::string& needle) {
  return text.find(needle) != std::string::npos;
}

}  // namespace

TEST_CASE(daemon_the_fake_drives_every_endpoint_the_real_daemon_has) {
  std::unique_ptr<DaemonClient> client = DaemonClient::create(fake_config());
  CHECK(client != nullptr);
  CHECK(client->connected());
  CHECK(client->last_error().empty());
  CHECK(contains(client->endpoint(), "fake"));

  std::string error;
  std::string version;
  CHECK(client->get_version(&version, &error));
  CHECK(!version.empty());

  const Config ours = default_config();

  json daemon_config;
  CHECK(client->get_config(&daemon_config, &error));
  // The two settings that decide whether audio can flow at all, and neither is
  // visible from anywhere else on the box: the shipped default interface is
  // `lo`, which never sees PTP or RTP, and auto_sinks_update can retarget a
  // sink this appliance wired deliberately.
  CHECK_EQ(daemon_config.at("interface_name").get<std::string>(),
           std::string("eth0"));
  CHECK_EQ(daemon_config.at("auto_sinks_update").get<bool>(), false);
  // The daemon's own frame size is the 1 ms period this project is built on.
  CHECK_EQ(daemon_config.at("tic_frame_size_at_1fs").get<int>(), 48);

  PtpStatus ptp;
  CHECK(client->get_ptp_status(&ptp, &error));
  CHECK_EQ(ptp.status, std::string("locked"));
  CHECK_EQ(ptp.gmid, std::string("6C-DF-FB-FF-FE-01-92-5C"));

  json sinks;
  json sources;
  CHECK(client->get_sinks(&sinks, &error));
  CHECK(client->get_sources(&sources, &error));
  // A fresh install holds nothing, which is what the Pi's daemon held.
  CHECK_EQ(sinks.at("sinks").size(), static_cast<size_t>(0));
  CHECK_EQ(sources.at("sources").size(), static_cast<size_t>(0));

  // Publish a block and read it back: the daemon's half of commissioning.
  CHECK(client->put_source(0, make_block_source(ours, ours.blocks[0]), &error));
  CHECK(client->get_sources(&sources, &error));
  CHECK_EQ(sources.at("sources").size(), static_cast<size_t>(1));
  CHECK_EQ(sources.at("sources").at(0).at("id").get<int>(), 0);

  // Subscribe to a stream on another block, and see the daemon say it is in use.
  json sink;
  CHECK(make_block_sink(ours.blocks[1], "v=0\r\ns=Remote\r\n", &sink, &error));
  CHECK(client->put_sink(1, sink, &error));
  SinkStatus status;
  CHECK(client->get_sink_status(1, &status, &error));
  CHECK(status.in_use);
  CHECK(status.receiving_rtp_packet);

  // Discovery, every kind, and the SDP the daemon would announce for our own
  // source.
  json discovered;
  CHECK(client->browse_sources("all", &discovered, &error));
  CHECK(!discovered.at("remote_sources").empty());
  CHECK(client->browse_sources("sap", &discovered, &error));
  CHECK(!discovered.at("remote_sources").empty());
  CHECK(client->browse_sources("mdns", &discovered, &error));
  // Nothing on the measured network is mDNS-announced. A fake that answered
  // every kind with the same list would hide a mistake in which kind we asked.
  CHECK(discovered.at("remote_sources").empty());

  std::string sdp;
  CHECK(client->get_source_sdp(0, &sdp, &error));
  CHECK(!sdp.empty());

  CHECK(client->delete_sink(1, &error));
  CHECK(client->delete_source(0, &error));
  CHECK(client->get_sinks(&sinks, &error));
  CHECK(client->get_sources(&sources, &error));
  CHECK_EQ(sinks.at("sinks").size(), static_cast<size_t>(0));
  CHECK_EQ(sources.at("sources").size(), static_cast<size_t>(0));
}

TEST_CASE(daemon_the_client_is_the_one_the_configuration_asks_for) {
  std::unique_ptr<DaemonClient> simulated = DaemonClient::create(fake_config());
  CHECK(contains(simulated->endpoint(), "fake"));

  // The real client against nothing at all: port 1 is tcpmux, and nothing in
  // this project's test environment listens on it. A daemon that is not there
  // has to be a reason to show an operator, not a crash and not a blank error.
  DaemonConfig real;
  real.address = "127.0.0.1";
  real.port = 1;
  std::unique_ptr<DaemonClient> http = DaemonClient::create(real);
  CHECK_EQ(http->endpoint(), std::string("127.0.0.1:1"));
  CHECK(!contains(http->endpoint(), "fake"));

  std::string error;
  json document;
  CHECK(!http->get_config(&document, &error));
  CHECK(!http->connected());
  CHECK(!error.empty());
  CHECK(!http->last_error().empty());
}

TEST_CASE(daemon_an_unlocked_ptp_slave_is_something_a_preflight_can_catch) {
  FakeDaemonClient fake(fake_config());
  std::string error;

  PtpStatus status;
  CHECK(fake.get_ptp_status(&status, &error));
  CHECK_EQ(status.status, std::string("locked"));

  // The preflight itself is the control module's (ticket 13). What this module
  // owes it is an honest answer: unlocked PTP is the commonest way an appliance
  // produces no audio while looking perfectly healthy, and a fake that could
  // only ever report `locked` would leave that check unprovable in CI.
  fake.set_ptp_status("unlocking");
  CHECK(fake.get_ptp_status(&status, &error));
  CHECK_EQ(status.status, std::string("unlocking"));
  CHECK(status.status != "locked");
}

TEST_CASE(daemon_a_sink_with_no_stream_is_not_the_daemon_failing) {
  // The daemon answers 400 or 404 on its per-stream paths for a stream it does
  // not hold, and that is an answer rather than a failure. Getting this wrong is
  // expensive in a way that is easy to miss: a status poll runs for every block,
  // so a false failure would flash a daemon error on unwired blocks forever.
  CHECK(DaemonClient::stream_absent(400));
  CHECK(DaemonClient::stream_absent(404));
  CHECK(!DaemonClient::stream_absent(200));
  CHECK(!DaemonClient::stream_absent(500));
  // Not even an HTTP status: a daemon that said nothing said nothing.
  CHECK(!DaemonClient::stream_absent(0));

  // And the fake gives the same observable answer as the HTTP client does for
  // those two statuses, so a caller cannot tell which one it is holding.
  FakeDaemonClient fake(fake_config());
  std::string error;

  SinkStatus status;
  CHECK(fake.get_sink_status(3, &status, &error));
  CHECK(!status.in_use);
  CHECK(!status.receiving_rtp_packet);
  CHECK(error.empty());
  CHECK(fake.connected());

  std::string sdp = "stale";
  CHECK(fake.get_source_sdp(3, &sdp, &error));
  CHECK(sdp.empty());
  CHECK(error.empty());
  CHECK(fake.connected());
}

TEST_CASE(daemon_a_block_maps_its_eight_device_channels_and_nothing_else) {
  const Config config = default_config();
  CHECK_EQ(config.blocks.size(), static_cast<size_t>(8));

  std::vector<int> mapped;
  for (size_t index = 0; index < config.blocks.size(); ++index) {
    const BlockConfig& block = config.blocks[index];
    const json source = make_block_source(config, block);

    // Eight, because AES67 allows no more than eight channels in a stream.
    CHECK_EQ(source.at("map").size(), static_cast<size_t>(8));

    // The map *is* the block-to-device mapping. Nothing derives it, so the test
    // asserts it against the configuration rather than against a convention.
    json expected = json::array();
    for (int channel = 0; channel < 8; ++channel) {
      expected.push_back(static_cast<int>(index) * 8 + channel);
    }
    CHECK_EQ(source.at("map"), expected);

    for (int channel : block.channels) {
      mapped.push_back(channel);
    }
  }

  // The eight maps partition the 64-channel device: if this ever stops being
  // true, one block carries a channel twice and the other carries silence, and
  // nothing else in the system would notice.
  std::sort(mapped.begin(), mapped.end());
  CHECK_EQ(mapped.size(), static_cast<size_t>(64));
  for (int channel = 0; channel < 64; ++channel) {
    CHECK_EQ(mapped[static_cast<size_t>(channel)], channel);
  }

  // The name comes from the index, not from the channels: an operator who
  // re-maps which device channels a block carries must not thereby rename the
  // stream the far end has already subscribed to.
  //
  // Bound to *values*, not to references: `make_block_source` returns a
  // temporary and `.at()` refers into it, so the macro's `const auto&` would
  // dangle past the end of the full expression. GCC 14 said so
  // (-Wdangling-reference) on the Pi, and it was right — the first version of
  // this comparison read freed memory and happened to work.
  BlockConfig remapped = config.blocks[2];
  std::swap(remapped.channels[0], remapped.channels[1]);
  const json remapped_name = make_block_source(config, remapped).at("name");
  const json original_name = make_block_source(config, config.blocks[2]).at("name");
  CHECK_EQ(remapped_name, original_name);

  CHECK_EQ(block_stream_name(0), std::string("aes67-srt block 0"));
  CHECK_EQ(block_stream_name(7), std::string("aes67-srt block 7"));
}

TEST_CASE(daemon_the_source_names_the_payload_the_wire_format_carries) {
  Config config = default_config();

  const json l24 = make_block_source(config, config.blocks[0]);
  CHECK_EQ(l24.at("codec").get<std::string>(), std::string("L24"));

  config.audio.format = "s16_le";
  const json l16 = make_block_source(config, config.blocks[0]);
  CHECK_EQ(l16.at("codec").get<std::string>(), std::string("L16"));

  // One millisecond at 48 kHz, which is three modules agreeing on one number
  // without being told about each other: the audio period, the daemon's packet
  // size, and the 1152 bytes of payload the wire format reserves per block.
  // If this ever fails, the daemon is being asked to packetise audio at a
  // cadence the rest of the system does not use.
  CHECK_EQ(l24.at("max_samples_per_packet").get<int>(), 48);
  CHECK_EQ(l24.at("max_samples_per_packet").get<int>(), config.audio.period_frames);
  CHECK_EQ(aes67_srt::audio::AudioFormat{}.period_frames, 48u);

  // Enabled even for a muted block: mute is this appliance's own business, and a
  // stream that vanishes from the far end's routing grid when somebody presses
  // mute is a call-out, not a mute.
  BlockConfig muted = config.blocks[1];
  muted.mute = true;
  CHECK_EQ(make_block_source(config, muted).at("enabled").get<bool>(), true);
}

TEST_CASE(daemon_a_source_document_carries_every_field_the_daemon_reads) {
  // The daemon reads a source with `pt.get<T>(...)` for each of these, and
  // `pt.get` *throws* when a node is missing. A document that omits one is
  // therefore not defaulted — it is refused, with the field named:
  //
  //   HTTP 400: error parsing JSON: No such node (ttl)
  //
  // That is what happened on the Pi on 2026-09-17, because the first version of
  // this document omitted the four fields at the end of this list, on the
  // reasoning that inventing a site's multicast policy was worse than omitting a
  // field. It was not: omitting was never the safe option, reading the daemon's
  // schema was. This test exists so that the next omission is caught here rather
  // than on somebody's hardware.
  //
  // Authority: `daemon/json.cpp`, `json_to_source` — bondagit-4.0.1 @ 68bd278 —
  // and the values asserted below are that function's own template's.
  const Config config = default_config();
  const json source = make_block_source(config, config.blocks[0]);

  for (const char* key :
       {"enabled", "name", "io", "map", "max_samples_per_packet", "codec",
        "address", "ttl", "payload_type", "dscp", "refclk_ptp_traceable"}) {
    CHECK(source.contains(key));
  }

  CHECK_EQ(source.at("ttl").get<int>(), 15);
  CHECK_EQ(source.at("payload_type").get<int>(), 98);  // paired with codec L24
  CHECK_EQ(source.at("dscp").get<int>(), 34);
  CHECK_EQ(source.at("refclk_ptp_traceable").get<bool>(), false);
  CHECK_EQ(source.at("codec").get<std::string>(), std::string("L24"));

  // L16 changes both the codec and the payload type together, because a document
  // that named one and not the other would describe a stream it is not.
  Config narrow = config;
  narrow.audio.format = "s16_le";
  const json l16 = make_block_source(narrow, narrow.blocks[0]);
  CHECK_EQ(l16.at("codec").get<std::string>(), std::string("L16"));
  CHECK_EQ(l16.at("payload_type").get<int>(), 97);
}

TEST_CASE(daemon_a_sink_document_carries_every_field_the_daemon_reads) {
  // The same rule for the sink — `daemon/json.cpp`, `json_to_sink` — and this
  // document happened to be complete from the start, which is worth asserting
  // rather than assuming: the source's four missing fields were invisible until
  // a real daemon refused them.
  const Config config = default_config();
  json sink;
  std::string error;
  CHECK(make_block_sink(config.blocks[0], "v=0\r\ns=Remote\r\n", &sink, &error));

  for (const char* key : {"name", "io", "source", "use_sdp", "sdp", "delay",
                          "ignore_refclk_gmid", "map"}) {
    CHECK(sink.contains(key));
  }
  // Non-zero, because it is the sink's receive buffer and not a delay line: zero
  // means no buffer at all, which is unusable even where a driver accepts it. The
  // value is the daemon's own template's.
  CHECK_EQ(sink.at("delay").get<int>(), 384);
}

TEST_CASE(daemon_a_sink_without_a_remote_sdp_is_refused_and_names_the_block) {
  const Config config = default_config();
  json sink;
  std::string error;

  CHECK(!make_block_sink(config.blocks[3], "", &sink, &error));
  CHECK(contains(error, "block 3"));
  CHECK(contains(error, "SDP"));
  CHECK(sink.is_null());  // refused means no document, not a half-built one

  CHECK(!make_block_sink(config.blocks[3], "  \r\n \t", &sink, &error));
  CHECK(contains(error, "block 3"));

  CHECK(make_block_sink(config.blocks[3], "v=0\r\ns=Remote\r\n", &sink, &error));
  CHECK(sink.is_object());
  CHECK_EQ(sink.at("map").size(), static_cast<size_t>(8));
  CHECK_EQ(sink.at("use_sdp").get<bool>(), true);
  CHECK_EQ(sink.at("sdp").get<std::string>(), std::string("v=0\r\ns=Remote\r\n"));

  // The sink's receive buffer, and it must not be zero: the driver refuses that
  // outright (measured 2026-09-17). It is not the A/V delay line — that is ours
  // (egress.delay_ms) — but the buffer that absorbs network jitter before the
  // audio is handed to ALSA, so the two are different things wearing one name.
  CHECK_EQ(sink.at("delay").get<int>(), 384);

  // A stream from another clock domain plays at the wrong rate and sounds
  // subtly wrong rather than obviously broken, so it is refused rather than
  // played: the safe default in both directions.
  CHECK_EQ(sink.at("ignore_refclk_gmid").get<bool>(), false);
}

TEST_CASE(daemon_the_commissioning_loopback_wires_a_sink_to_our_own_source) {
  // Ticket 09's commissioning loopback, at the daemon seam: publish a block,
  // read back the SDP the daemon announces for it, subscribe another block to
  // that SDP, and see the daemon report a stream in use. This is the half of
  // the loopback that needs no hardware, no kernel module and no network — the
  // half the audio module's null backend cannot reach.
  const Config config = default_config();
  std::unique_ptr<DaemonClient> client = DaemonClient::create(fake_config());
  std::string error;

  CHECK(client->put_source(0, make_block_source(config, config.blocks[0]), &error));

  std::string sdp;
  CHECK(client->get_source_sdp(0, &sdp, &error));
  CHECK(contains(sdp, "L24/48000/8"));
  CHECK(contains(sdp, "a=ptime:1"));

  // AES67 writes a sender's SDP from the receiver's point of view, so a
  // sender's own description declares `recvonly`. It looks like a mistake and
  // is not, and knowing that is what stops somebody "fixing" it.
  CHECK(contains(sdp, "a=recvonly"));

  json sink;
  CHECK(make_block_sink(config.blocks[1], sdp, &sink, &error));
  CHECK(client->put_sink(1, sink, &error));
  CHECK_EQ(sink.at("map").size(), static_cast<size_t>(8));

  SinkStatus status;
  CHECK(client->get_sink_status(1, &status, &error));
  CHECK(status.in_use);
  CHECK(status.receiving_rtp_packet);
  CHECK(status.muted == false);

  // The eight-channel sender on the measured network is exactly one of our
  // blocks — L24, 48 kHz, eight channels, 1 ms — which is why the first real
  // audio test needs no second appliance.
  json discovered;
  CHECK(client->browse_sources("all", &discovered, &error));
  bool found_the_eight_channel_sender = false;
  for (const json& entry : discovered.at("remote_sources")) {
    const std::string entry_sdp = entry.at("sdp").get<std::string>();
    if (contains(entry_sdp, "L24/48000/8")) {
      found_the_eight_channel_sender = true;
      CHECK_EQ(entry.at("name").get<std::string>(), std::string("AES67-TX-1"));
      CHECK_EQ(entry.at("address").get<std::string>(), std::string("10.10.80.20"));
    }
  }
  CHECK(found_the_eight_channel_sender);

  // Two senders, one of them mono. Getting the wrong one is a visible mistake
  // rather than a silently half-empty stream.
  json all_kinds;
  CHECK(client->browse_sources("sap", &all_kinds, &error));
  CHECK_EQ(all_kinds.at("remote_sources").size(), static_cast<size_t>(2));
}
