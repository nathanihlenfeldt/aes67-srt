#include "config.hpp"

#include <nlohmann/json.hpp>

#include <fstream>
#include <string>
#include <vector>

#include "util.hpp"

namespace aes67_srt {
namespace {

using nlohmann::json;

bool fail(std::string* reason, const std::string& message) {
  if (reason != nullptr) {
    *reason = message;
  }
  return false;
}

/**
 * Every key in |object| must be in |allowed|.
 *
 * A typo in a key name is the commonest way an appliance ends up running on
 * defaults while looking configured, so an unknown key is refused rather than
 * ignored.  The message carries the path, because "unknown key" on its own
 * sends the operator hunting through the whole file.
 */
bool check_keys(const json& object, const std::vector<std::string>& allowed,
                const std::string& prefix, std::string* reason) {
  for (auto it = object.begin(); it != object.end(); ++it) {
    bool known = false;
    for (const std::string& key : allowed) {
      if (key == it.key()) {
        known = true;
        break;
      }
    }
    if (!known) {
      return fail(reason, "unknown key \"" + prefix + it.key() + "\"");
    }
  }
  return true;
}

/** Fetch an optional object member. Absent is fine; the wrong type is not. */
bool want_object(const json& parent, const char* key, const std::string& path,
                 const json** out, std::string* reason) {
  if (!parent.contains(key)) {
    return true;
  }
  const json& value = parent.at(key);
  if (!value.is_object()) {
    return fail(reason, path + key + ": expected an object");
  }
  *out = &value;
  return true;
}

bool read_int(const json& parent, const char* key, const std::string& path,
              int* out, std::string* reason) {
  if (!parent.contains(key)) {
    return true;
  }
  const json& value = parent.at(key);
  if (!value.is_number_integer()) {
    return fail(reason, path + key + ": expected an integer");
  }
  *out = value.get<int>();
  return true;
}

bool read_double(const json& parent, const char* key, const std::string& path,
                 double* out, std::string* reason) {
  if (!parent.contains(key)) {
    return true;
  }
  const json& value = parent.at(key);
  if (!value.is_number()) {
    return fail(reason, path + key + ": expected a number");
  }
  *out = value.get<double>();
  return true;
}

bool read_bool(const json& parent, const char* key, const std::string& path,
               bool* out, std::string* reason) {
  if (!parent.contains(key)) {
    return true;
  }
  const json& value = parent.at(key);
  if (!value.is_boolean()) {
    return fail(reason, path + key + ": expected true or false");
  }
  *out = value.get<bool>();
  return true;
}

bool read_string(const json& parent, const char* key, const std::string& path,
                 std::string* out, std::string* reason) {
  if (!parent.contains(key)) {
    return true;
  }
  const json& value = parent.at(key);
  if (!value.is_string()) {
    return fail(reason, path + key + ": expected a string");
  }
  *out = value.get<std::string>();
  return true;
}

bool read_audio(const json& document, AudioConfig* audio, std::string* reason) {
  const json* object = nullptr;
  if (!want_object(document, "audio", "", &object, reason)) {
    return false;
  }
  if (object == nullptr) {
    return true;
  }
  if (!check_keys(*object,
                  {"backend", "device", "channels", "sample_rate", "format",
                   "period_frames", "periods"},
                  "audio.", reason)) {
    return false;
  }
  const std::string path = "audio.";
  return read_string(*object, "backend", path, &audio->backend, reason) &&
         read_string(*object, "device", path, &audio->device, reason) &&
         read_int(*object, "channels", path, &audio->channels, reason) &&
         read_int(*object, "sample_rate", path, &audio->sample_rate, reason) &&
         read_string(*object, "format", path, &audio->format, reason) &&
         read_int(*object, "period_frames", path, &audio->period_frames, reason) &&
         read_int(*object, "periods", path, &audio->periods, reason);
}

bool read_link(const json& document, LinkConfig* link, std::string* reason) {
  const json* object = nullptr;
  if (!want_object(document, "link", "", &object, reason)) {
    return false;
  }
  if (object == nullptr) {
    return true;
  }
  if (!check_keys(
          *object,
          {"role", "mode", "peer", "local_port", "latency_ms", "alarm_delay_ms",
           "passphrase", "blocks", "receive_buffer_bytes", "flow_control_packets"},
          "link.", reason)) {
    return false;
  }
  const std::string path = "link.";
  return read_string(*object, "role", path, &link->role, reason) &&
         read_string(*object, "mode", path, &link->mode, reason) &&
         read_string(*object, "peer", path, &link->peer, reason) &&
         read_int(*object, "local_port", path, &link->local_port, reason) &&
         read_int(*object, "latency_ms", path, &link->latency_ms, reason) &&
         read_int(*object, "alarm_delay_ms", path, &link->alarm_delay_ms, reason) &&
         read_string(*object, "passphrase", path, &link->passphrase, reason) &&
         read_int(*object, "blocks", path, &link->blocks, reason) &&
         read_int(*object, "receive_buffer_bytes", path,
                  &link->receive_buffer_bytes, reason) &&
         read_int(*object, "flow_control_packets", path,
                  &link->flow_control_packets, reason);
}

bool read_blocks(const json& document, Config* config, std::string* reason) {
  if (!document.contains("blocks")) {
    // Absent means "the conventional mapping": block i takes channels 8i..8i+7.
    config->fill_default_blocks();
    return true;
  }
  if (!document.at("blocks").is_array()) {
    return fail(reason, "blocks: expected an array");
  }

  config->blocks.clear();
  int position = 0;
  for (const json& entry : document.at("blocks")) {
    const std::string path = "blocks[" + std::to_string(position) + "].";
    if (!entry.is_object()) {
      return fail(reason, path + "expected an object");
    }
    if (!check_keys(entry, {"index", "channels", "gain_db", "mute"}, path,
                    reason)) {
      return false;
    }
    BlockConfig block;
    block.index = position;
    if (!read_int(entry, "index", path, &block.index, reason) ||
        !read_double(entry, "gain_db", path, &block.gain_db, reason) ||
        !read_bool(entry, "mute", path, &block.mute, reason)) {
      return false;
    }
    if (!entry.contains("channels")) {
      return fail(reason, path + "channels: required");
    }
    if (!entry.at("channels").is_array()) {
      return fail(reason, path + "channels: expected an array");
    }
    for (const json& channel : entry.at("channels")) {
      if (!channel.is_number_integer()) {
        return fail(reason, path + "channels: expected integers");
      }
      block.channels.push_back(channel.get<int>());
    }
    config->blocks.push_back(block);
    ++position;
  }
  return true;
}

}  // namespace

bool parse_config(const std::string& json_text, Config* config,
                  std::string* reason) {
  if (config == nullptr) {
    return fail(reason, "no configuration to fill");
  }

  json document;
  try {
    document = json::parse(json_text);
  } catch (const json::exception& error) {
    return fail(reason, std::string("not valid JSON: ") + error.what());
  }
  if (!document.is_object()) {
    return fail(reason, "the configuration must be a JSON object");
  }
  if (!check_keys(document,
                  {"version", "audio", "link", "aes67_daemon", "egress", "blocks",
                   "http_addr", "http_port"},
                  "", reason)) {
    return false;
  }

  Config parsed;
  if (!read_int(document, "version", "", &parsed.version, reason) ||
      !read_audio(document, &parsed.audio, reason) ||
      !read_link(document, &parsed.link, reason)) {
    return false;
  }

  const json* daemon = nullptr;
  if (!want_object(document, "aes67_daemon", "", &daemon, reason)) {
    return false;
  }
  if (daemon != nullptr &&
      (!check_keys(*daemon, {"address", "port", "fake"}, "aes67_daemon.", reason) ||
       !read_string(*daemon, "address", "aes67_daemon.", &parsed.daemon.address,
                    reason) ||
       !read_int(*daemon, "port", "aes67_daemon.", &parsed.daemon.port, reason) ||
       !read_bool(*daemon, "fake", "aes67_daemon.", &parsed.daemon.fake, reason))) {
    return false;
  }

  const json* egress = nullptr;
  if (!want_object(document, "egress", "", &egress, reason)) {
    return false;
  }
  if (egress != nullptr &&
      (!check_keys(*egress, {"delay_ms", "test_signal_channel"}, "egress.",
                   reason) ||
       !read_double(*egress, "delay_ms", "egress.", &parsed.egress.delay_ms,
                    reason) ||
       !read_int(*egress, "test_signal_channel", "egress.",
                 &parsed.egress.test_signal_channel, reason))) {
    return false;
  }

  if (!read_blocks(document, &parsed, reason) ||
      !read_string(document, "http_addr", "", &parsed.http_addr, reason) ||
      !read_int(document, "http_port", "", &parsed.http_port, reason)) {
    return false;
  }

  if (!parsed.validate(reason)) {
    return false;
  }
  *config = parsed;
  return true;
}

void Config::fill_default_blocks() {
  blocks.clear();
  for (int index = 0; index < link.blocks; ++index) {
    BlockConfig block;
    block.index = index;
    for (int channel = 0; channel < 8; ++channel) {
      block.channels.push_back(index * 8 + channel);
    }
    blocks.push_back(block);
  }
}

std::string Config::to_json() const {
  json document;
  document["version"] = version;

  document["audio"] = {
      {"backend", audio.backend},   {"device", audio.device},
      {"channels", audio.channels}, {"sample_rate", audio.sample_rate},
      {"format", audio.format},     {"period_frames", audio.period_frames},
      {"periods", audio.periods}};

  document["link"] = {{"role", link.role},
                      {"mode", link.mode},
                      {"peer", link.peer},
                      {"local_port", link.local_port},
                      {"latency_ms", link.latency_ms},
                      {"alarm_delay_ms", link.alarm_delay_ms},
                      {"passphrase", link.passphrase},
                      {"blocks", link.blocks},
                      {"receive_buffer_bytes", link.receive_buffer_bytes},
                      {"flow_control_packets", link.flow_control_packets}};

  document["aes67_daemon"] = {
      {"address", daemon.address}, {"port", daemon.port}, {"fake", daemon.fake}};

  document["egress"] = {{"delay_ms", egress.delay_ms},
                        {"test_signal_channel", egress.test_signal_channel}};

  json block_array = json::array();
  for (const BlockConfig& block : blocks) {
    block_array.push_back({{"index", block.index},
                           {"channels", block.channels},
                           {"gain_db", block.gain_db},
                           {"mute", block.mute}});
  }
  document["blocks"] = block_array;

  document["http_addr"] = http_addr;
  document["http_port"] = http_port;
  return document.dump(2) + "\n";
}

bool Config::validate(std::string* reason) const {
  if (version != 1) {
    return fail(reason, "version: expected 1, got " + std::to_string(version));
  }

  // --- audio ---------------------------------------------------------------
  if (audio.backend != "ravenna" && audio.backend != "null") {
    return fail(reason, "audio.backend: expected \"ravenna\" or \"null\", got \"" +
                            audio.backend + "\"");
  }
  if (audio.device.empty()) {
    return fail(reason, "audio.device: required (e.g. plughw:RAVENNA)");
  }
  if (audio.sample_rate != 48000) {
    return fail(reason, "audio.sample_rate: this build carries 48 kHz only, got " +
                            std::to_string(audio.sample_rate));
  }
  if (audio.channels < 8 || audio.channels > 64) {
    return fail(reason, "audio.channels: expected 8..64, got " +
                            std::to_string(audio.channels));
  }
  if (audio.channels % 8 != 0) {
    return fail(reason,
                "audio.channels: must be a multiple of 8, the AES67 stream "
                "size; got " +
                    std::to_string(audio.channels));
  }
  if (audio.format != "s24_3le" && audio.format != "s16_le") {
    return fail(reason, "audio.format: expected \"s24_3le\" or \"s16_le\", got \"" +
                            audio.format + "\"");
  }
  const int one_ms = audio.sample_rate / 1000;
  if (audio.period_frames != one_ms) {
    return fail(reason, "audio.period_frames: must be one millisecond at " +
                            std::to_string(audio.sample_rate) + " Hz (" +
                            std::to_string(one_ms) + "), got " +
                            std::to_string(audio.period_frames));
  }
  if (audio.periods < 2 || audio.periods > 64) {
    return fail(reason, "audio.periods: expected 2..64, got " +
                            std::to_string(audio.periods));
  }

  // --- link ----------------------------------------------------------------
  const std::string role = to_lower(link.role);
  if (role != "tx" && role != "rx" && role != "duplex") {
    return fail(reason, "link.role: expected \"tx\", \"rx\" or \"duplex\", got \"" +
                            link.role + "\"");
  }
  const std::string mode = to_lower(link.mode);
  if (mode != "caller" && mode != "listener" && mode != "rendezvous" &&
      mode != "loopback") {
    return fail(reason,
                "link.mode: expected \"caller\", \"listener\", \"rendezvous\" or "
                "\"loopback\", got \"" +
                    link.mode + "\"");
  }
  std::string peer_host;
  int peer_port = 0;
  // A listener waits for someone to arrive and a loopback has no peer at all, so
  // neither may be required to name one.
  if (mode != "listener" && mode != "loopback" &&
      !split_host_port(link.peer, &peer_host, &peer_port)) {
    return fail(reason, "link.peer: expected host:port for mode \"" + mode +
                            "\", got \"" + link.peer + "\"");
  }
  if (link.local_port < 1 || link.local_port > 65535) {
    return fail(reason, "link.local_port: expected 1..65535, got " +
                            std::to_string(link.local_port));
  }
  if (link.latency_ms < 20 || link.latency_ms > 8000) {
    return fail(reason, "link.latency_ms: expected 20..8000, got " +
                            std::to_string(link.latency_ms));
  }
  if (link.alarm_delay_ms <= link.latency_ms) {
    return fail(reason, "link.alarm_delay_ms: must exceed link.latency_ms (" +
                            std::to_string(link.latency_ms) + "), got " +
                            std::to_string(link.alarm_delay_ms));
  }
  if (!link.passphrase.empty() &&
      (link.passphrase.size() < 10 || link.passphrase.size() > 79)) {
    return fail(reason, "link.passphrase: must be 10..79 characters, got " +
                            std::to_string(link.passphrase.size()));
  }
  const int expected_blocks = audio.channels / 8;
  if (link.blocks < 1 || link.blocks > 8) {
    return fail(reason,
                "link.blocks: expected 1..8, the most the wire format carries; "
                "got " +
                    std::to_string(link.blocks));
  }
  if (link.blocks != expected_blocks) {
    return fail(reason, "link.blocks: " + std::to_string(link.blocks) +
                            " blocks do not cover audio.channels " +
                            std::to_string(audio.channels) + "; expected " +
                            std::to_string(expected_blocks));
  }
  // Buffer tuning: zero means the library's default, which is what a site should
  // run with until the values have been tuned against a real link.
  if (link.receive_buffer_bytes != 0 &&
      (link.receive_buffer_bytes < 1024 || link.receive_buffer_bytes > 268435456)) {
    return fail(reason,
                "link.receive_buffer_bytes: expected 0 (library default) or "
                "1024..268435456, got " +
                    std::to_string(link.receive_buffer_bytes));
  }
  if (link.flow_control_packets != 0 &&
      (link.flow_control_packets < 2 || link.flow_control_packets > 1000000)) {
    return fail(reason,
                "link.flow_control_packets: expected 0 (library default) or "
                "2..1000000, got " +
                    std::to_string(link.flow_control_packets));
  }

  // --- daemon --------------------------------------------------------------
  if (daemon.address.empty()) {
    return fail(reason, "aes67_daemon.address: required");
  }
  if (daemon.port < 1 || daemon.port > 65535) {
    return fail(reason, "aes67_daemon.port: expected 1..65535, got " +
                            std::to_string(daemon.port));
  }

  // --- blocks --------------------------------------------------------------
  if (static_cast<int>(blocks.size()) != link.blocks) {
    return fail(reason, "blocks: expected " + std::to_string(link.blocks) +
                            " entries to match link.blocks, got " +
                            std::to_string(blocks.size()));
  }
  std::vector<int> index_seen(static_cast<size_t>(link.blocks), 0);
  std::vector<int> channel_seen(static_cast<size_t>(audio.channels), 0);
  for (const BlockConfig& block : blocks) {
    const std::string path = "blocks[" + std::to_string(block.index) + "].";
    if (block.index < 0 || block.index >= link.blocks) {
      return fail(reason, path + "index: expected 0.." +
                              std::to_string(link.blocks - 1) + ", got " +
                              std::to_string(block.index));
    }
    if (index_seen[static_cast<size_t>(block.index)]++ != 0) {
      return fail(reason, path + "index: duplicate block index " +
                              std::to_string(block.index));
    }
    if (block.channels.size() != 8) {
      return fail(reason,
                  path +
                      "channels: expected exactly 8, the AES67 stream size; got " +
                      std::to_string(block.channels.size()));
    }
    for (int channel : block.channels) {
      if (channel < 0 || channel >= audio.channels) {
        return fail(reason, path + "channels: " + std::to_string(channel) +
                                " is outside audio.channels 0.." +
                                std::to_string(audio.channels - 1));
      }
      if (channel_seen[static_cast<size_t>(channel)]++ != 0) {
        return fail(reason, path + "channels: device channel " +
                                std::to_string(channel) +
                                " is already carried by another block");
      }
    }
    if (block.gain_db < -60.0 || block.gain_db > 24.0) {
      return fail(reason, path + "gain_db: expected -60..+24 dB, got " +
                              std::to_string(block.gain_db));
    }
  }

  // --- egress --------------------------------------------------------------
  if (egress.delay_ms < 0.0 || egress.delay_ms > 5000.0) {
    return fail(reason, "egress.delay_ms: expected 0..5000 ms, got " +
                            std::to_string(egress.delay_ms) +
                            " (audio can only be delayed, never advanced)");
  }
  if (egress.test_signal_channel < -1 ||
      egress.test_signal_channel >= audio.channels) {
    return fail(reason,
                "egress.test_signal_channel: expected -1 (off) or a device "
                "channel 0.." +
                    std::to_string(audio.channels - 1) + ", got " +
                    std::to_string(egress.test_signal_channel));
  }

  // --- http ----------------------------------------------------------------
  if (http_addr.empty()) {
    return fail(reason, "http_addr: required");
  }
  if (http_port < 1 || http_port > 65535) {
    return fail(reason,
                "http_port: expected 1..65535, got " + std::to_string(http_port));
  }
  return true;
}

bool load_config(const std::string& path, Config* config, std::string* reason) {
  std::ifstream file(path);
  if (!file) {
    return fail(reason, "cannot read " + path);
  }
  const std::string text((std::istreambuf_iterator<char>(file)),
                         std::istreambuf_iterator<char>());
  std::string detail;
  if (!parse_config(text, config, &detail)) {
    return fail(reason, path + ": " + detail);
  }
  return true;
}

}  // namespace aes67_srt
