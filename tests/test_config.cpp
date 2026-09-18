#include <nlohmann/json.hpp>

#include <string>

#include "config.hpp"
#include "test_framework.hpp"

namespace {

using nlohmann::json;

/** The shape the specification describes: 64 channels, eight blocks. */
json sample_document() {
  json document;
  document["version"] = 1;
  document["audio"] = {{"backend", "ravenna"}, {"device", "plughw:RAVENNA"},
                       {"channels", 64},       {"sample_rate", 48000},
                       {"format", "s24_3le"},  {"period_frames", 48},
                       {"periods", 8}};
  document["link"] = {
      {"role", "duplex"},   {"mode", "rendezvous"}, {"peer", "203.0.113.10:9000"},
      {"local_port", 9000}, {"latency_ms", 120},    {"alarm_delay_ms", 500},
      {"passphrase", ""},   {"blocks", 8}};
  document["aes67_daemon"] = {
      {"address", "127.0.0.1"}, {"port", 8080}, {"fake", false}};
  document["egress"] = {{"delay_ms", 0.0}, {"test_signal_channel", -1}};
  document["http_addr"] = "0.0.0.0";
  document["http_port"] = 8082;
  return document;
}

/** Parse expecting refusal, and return the reason. */
std::string refusal(const json& document) {
  aes67_srt::Config config;
  std::string reason;
  CHECK(!aes67_srt::parse_config(document.dump(), &config, &reason));
  return reason;
}

/**
 * A refusal is only useful if it names the field.  Every message starts with the
 * path of the offending key, so this is the assertion that keeps them honest.
 */
bool names(const std::string& reason, const std::string& field) {
  return reason.rfind(field, 0) == 0;
}

}  // namespace

TEST_CASE(config_accepts_the_specified_shape) {
  aes67_srt::Config config;
  std::string reason;
  CHECK(aes67_srt::parse_config(sample_document().dump(), &config, &reason));
  CHECK_EQ(config.audio.channels, 64);
  CHECK_EQ(config.link.blocks, 8);
  CHECK_EQ(static_cast<int>(config.blocks.size()), 8);
  CHECK_EQ(config.http_port, 8082);
}

TEST_CASE(config_fills_the_conventional_block_mapping) {
  // No "blocks" key: block i takes device channels 8i..8i+7.
  aes67_srt::Config config;
  std::string reason;
  CHECK(aes67_srt::parse_config(sample_document().dump(), &config, &reason));
  CHECK_EQ(static_cast<int>(config.blocks.size()), 8);
  CHECK_EQ(static_cast<int>(config.blocks[3].channels.size()), 8);
  CHECK_EQ(config.blocks[3].channels[0], 24);
  CHECK_EQ(config.blocks[7].channels[7], 63);
}

TEST_CASE(config_refuses_an_unknown_key_and_names_it) {
  json document = sample_document();
  document["link"]["latency"] = 120;  // a typo for latency_ms
  const std::string reason = refusal(document);
  CHECK(names(reason, "unknown key \"link.latency\""));
}

TEST_CASE(config_refuses_channels_that_are_not_whole_blocks) {
  json document = sample_document();
  document["audio"]["channels"] = 12;  // one and a half blocks
  document["link"]["blocks"] = 1;
  CHECK(names(refusal(document), "audio.channels"));
}

TEST_CASE(config_refuses_blocks_that_do_not_cover_the_channels) {
  json document = sample_document();
  document["link"]["blocks"] = 4;
  CHECK(names(refusal(document), "link.blocks"));
}

TEST_CASE(config_refuses_a_sample_rate_it_cannot_carry) {
  json document = sample_document();
  document["audio"]["sample_rate"] = 96000;
  CHECK(names(refusal(document), "audio.sample_rate"));
}

TEST_CASE(config_refuses_periods_that_are_not_one_millisecond) {
  json document = sample_document();
  document["audio"]["period_frames"] = 480;
  CHECK(names(refusal(document), "audio.period_frames"));
}

TEST_CASE(config_refuses_an_alarm_below_the_latency) {
  json document = sample_document();
  document["link"]["alarm_delay_ms"] = 100;
  CHECK(names(refusal(document), "link.alarm_delay_ms"));
}

TEST_CASE(config_refuses_an_unknown_role) {
  json document = sample_document();
  document["link"]["role"] = "transmitter";
  CHECK(names(refusal(document), "link.role"));
}

TEST_CASE(config_refuses_a_peer_without_a_port) {
  json document = sample_document();
  document["link"]["peer"] = "203.0.113.10";
  CHECK(names(refusal(document), "link.peer"));
}

TEST_CASE(config_accepts_a_listener_with_no_peer) {
  json document = sample_document();
  document["link"]["mode"] = "listener";
  document["link"]["peer"] = "";
  aes67_srt::Config config;
  std::string reason;
  CHECK(aes67_srt::parse_config(document.dump(), &config, &reason));
}

TEST_CASE(config_refuses_a_flow_control_window_libsrt_would_reject) {
  // 31 is a configuration libsrt refuses with "SRTO_FC: minimum allowed value is
  // 32", and the whole point of validating before applying is that the operator
  // gets a sentence naming the field rather than a library's "Bad parameters"
  // from somewhere inside the transport. Measured against libsrt 1.5.7: the
  // floor is not in the header and no document here recorded it.
  json document = sample_document();
  document["link"]["flow_control_packets"] = 31;
  const std::string reason = refusal(document);
  CHECK(names(reason, "link.flow_control_packets"));
  CHECK(reason.find("32") != std::string::npos);

  // And the floor itself is accepted, so the boundary is where the library puts
  // it rather than one past it.
  document["link"]["flow_control_packets"] = 32;
  aes67_srt::Config config;
  std::string accepted_reason;
  CHECK(aes67_srt::parse_config(document.dump(), &config, &accepted_reason));
  CHECK_EQ(config.link.flow_control_packets, 32);
}

TEST_CASE(config_accepts_a_loopback_with_no_peer) {
  // The in-process loopback has no peer to name: it is this process's own byte
  // pipe, which is what lets the appliance run end to end with nothing attached.
  json document = sample_document();
  document["link"]["mode"] = "loopback";
  document["link"]["peer"] = "";
  aes67_srt::Config config;
  std::string reason;
  CHECK(aes67_srt::parse_config(document.dump(), &config, &reason));
  CHECK_EQ(config.link.mode, std::string("loopback"));
}

TEST_CASE(config_refuses_a_mode_it_does_not_have) {
  json document = sample_document();
  document["link"]["mode"] = "peer-to-peer";
  const std::string reason = refusal(document);
  CHECK(names(reason, "link.mode"));
  // The message lists what *is* accepted, so a typo is answered with the set of
  // right answers rather than only with the wrong one.
  CHECK(reason.find("loopback") != std::string::npos);
}

TEST_CASE(config_refuses_a_passphrase_that_is_too_short) {
  json document = sample_document();
  document["link"]["passphrase"] = "short";
  CHECK(names(refusal(document), "link.passphrase"));
}

TEST_CASE(config_refuses_one_device_channel_carried_twice) {
  json document = sample_document();
  document["blocks"] = json::array();
  for (int block = 0; block < 8; ++block) {
    json channels = json::array();
    for (int channel = 0; channel < 8; ++channel) {
      channels.push_back(block * 8 + channel);
    }
    document["blocks"].push_back({{"index", block},
                                  {"channels", channels},
                                  {"gain_db", 0.0},
                                  {"mute", false}});
  }
  document["blocks"][5]["channels"][0] = 0;  // also carried by block 0
  const std::string reason = refusal(document);
  CHECK(names(reason, "blocks[5].channels"));
  CHECK(reason.find("already carried") != std::string::npos);
}

TEST_CASE(config_refuses_a_block_that_is_not_eight_channels) {
  json document = sample_document();
  document["blocks"] = json::array();
  document["blocks"].push_back({{"index", 0},
                                {"channels", {0, 1, 2, 3}},
                                {"gain_db", 0.0},
                                {"mute", false}});
  CHECK(names(refusal(document), "blocks"));
}

TEST_CASE(config_refuses_an_opus_bitrate_the_encoder_cannot_reach) {
  // The measured ceiling for an 8-mono-stream block is ~256 kbit/s per channel;
  // 510000 is a number the encoder does not reach, so accepting it would be the
  // configuration saying one thing and the encoder doing another.
  json document = sample_document();
  document["audio"]["period_frames"] = 960;  // an Opus frame, not 1 ms
  json blocks = json::array();
  for (int index = 0; index < 8; ++index) {
    json channels = json::array();
    for (int channel = 0; channel < 8; ++channel) {
      channels.push_back(index * 8 + channel);
    }
    blocks.push_back({{"index", index},
                      {"channels", channels},
                      {"gain_db", 0.0},
                      {"mute", false},
                      {"codec", "opus"},
                      {"bitrate_bps_per_channel", 510000}});
  }
  document["blocks"] = blocks;
  const std::string reason = refusal(document);
  CHECK(names(reason, "blocks[0].bitrate_bps_per_channel"));
  CHECK(reason.find("256000") != std::string::npos);

  // A reachable rate is accepted, and the same document parses.
  for (json& block : document["blocks"]) {
    block["bitrate_bps_per_channel"] = 128000;
  }
  aes67_srt::Config config;
  std::string error;
  CHECK(aes67_srt::parse_config(document.dump(), &config, &error));
}

TEST_CASE(config_refuses_an_opus_block_with_a_one_millisecond_period) {
  // Opus cannot take a 1 ms frame, so the period *is* the codec frame in codec
  // mode. A 48-frame period with an Opus block is a configuration that would not
  // work, and it is refused by name.
  json document = sample_document();
  document["audio"]["period_frames"] = 48;
  json blocks = json::array();
  for (int index = 0; index < 8; ++index) {
    json channels = json::array();
    for (int channel = 0; channel < 8; ++channel) {
      channels.push_back(index * 8 + channel);
    }
    blocks.push_back({{"index", index},
                      {"channels", channels},
                      {"gain_db", 0.0},
                      {"mute", false},
                      {"codec", "opus"},
                      {"bitrate_bps_per_channel", 128000}});
  }
  document["blocks"] = blocks;
  CHECK(names(refusal(document), "audio.period_frames"));
}

TEST_CASE(config_refuses_a_delay_that_would_advance_audio) {
  json document = sample_document();
  document["egress"]["delay_ms"] = -10.0;
  CHECK(names(refusal(document), "egress.delay_ms"));
}

TEST_CASE(config_refuses_a_missing_file) {
  aes67_srt::Config config;
  std::string reason;
  CHECK(!aes67_srt::load_config("/nonexistent/aes67-srt.conf", &config, &reason));
  CHECK(reason.find("cannot read") != std::string::npos);
}

TEST_CASE(config_round_trips_through_json) {
  aes67_srt::Config first;
  std::string reason;
  CHECK(aes67_srt::parse_config(sample_document().dump(), &first, &reason));

  aes67_srt::Config second;
  CHECK(aes67_srt::parse_config(first.to_json(), &second, &reason));
  CHECK_EQ(second.audio.channels, first.audio.channels);
  CHECK_EQ(second.link.blocks, first.link.blocks);
  CHECK_EQ(second.link.latency_ms, first.link.latency_ms);
  CHECK_EQ(static_cast<int>(second.blocks.size()),
           static_cast<int>(first.blocks.size()));
  CHECK_EQ(second.blocks[2].channels[3], first.blocks[2].channels[3]);
}
