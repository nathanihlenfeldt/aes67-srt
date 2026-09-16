#include "aes67/fake_daemon_client.hpp"

#include <sstream>

#include "log.hpp"

namespace aes67_srt::daemon {
namespace {

using nlohmann::json;

/**
 * The grandmaster a provisioned Pi is locked to
 * (`docs/research/aes67-daemon-64ch.md`).  Both senders on that network
 * reference it, which is what makes the bench a single clock domain.
 */
constexpr const char* k_gmid = "6C-DF-FB-FF-FE-01-92-5C";

/** The loopback address the daemon's own sources are announced from. */
constexpr const char* k_rtp_mcast_base = "239.1.0.1";
constexpr int k_rtp_port = 5004;

/**
 * One AES67 sender's SDP.
 *
 * Written the way AES67 writes it, including `a=recvonly` on a *sender*.  That
 * looks like a mistake and is not: AES67's convention is to express the sender's
 * SDP from the receiver's point of view, so the announcements on the measured
 * network declare it and anything that "fixed" it would be wrong.
 *
 * The payload type number is copied, not interpreted.  This project never parses
 * SDP — the daemon does, and a sink hands it the document verbatim — so the
 * number only has to be present and consistent with the `rtpmap` beside it.
 */
std::string sdp_for(const std::string& name, const std::string& multicast,
                    int channels, unsigned ssrc, const std::string& codec) {
  std::ostringstream out;
  out << "v=0\r\n"
      << "o=- " << ssrc << " 0 IN IP4 127.0.0.1\r\n"
      << "s=" << name << "\r\n"
      << "c=IN IP4 " << multicast << "/32\r\n"
      << "t=0 0\r\n"
      << "a=clock-domain:PTPv2 0\r\n"
      << "m=audio " << k_rtp_port << " RTP/AVP 98\r\n"
      << "a=rtpmap:98 " << codec << "/48000/" << channels << "\r\n"
      << "a=sync-time:0\r\n"
      << "a=framecount:48\r\n"
      << "a=ptime:1\r\n"
      << "a=mediaclk:direct=0\r\n"
      << "a=ts-refclk:ptp=IEEE1588-2008:" << k_gmid << ":0\r\n"
      << "a=recvonly\r\n";
  return out.str();
}

/** The multicast address the daemon would allocate to source |id|. */
std::string next_multicast(int id) {
  return "239.1.0." + std::to_string(id + 1);
}

}  // namespace

FakeDaemonClient::FakeDaemonClient(const DaemonConfig& config) : config_(config) {
  log().write(LogLevel::warn,
              "using the simulated AES67 daemon (aes67_daemon.fake): no audio "
              "reaches the network");

  ptp_.status = "locked";
  ptp_.gmid = k_gmid;
  ptp_.jitter = 9;

  // The configuration a provisioned Pi actually reports, field for field
  // (docs/research/aes67-daemon-64ch.md).  Two of these are load-bearing rather
  // than cosmetic: interface_name must not be the shipped default of `lo`, and
  // auto_sinks_update must be off or the daemon can retarget a sink this
  // appliance wired deliberately.
  config_json_ = json{
      {"interface_name", "eth0"},
      {"tic_frame_size_at_1fs", 48},
      {"sample_rate", 48000},
      {"max_tic_frame_size", 1024},
      {"rtp_mcast_base", k_rtp_mcast_base},
      {"rtp_port", k_rtp_port},
      {"ptp_domain", 0},
      {"sap_interval", 30},
      {"streamer_enabled", false},
      {"auto_sinks_update", false},
      {"nmos_enabled", false},
      {"http_port", config_.port},
  };
}

bool FakeDaemonClient::connected() const {
  return true;
}

std::string FakeDaemonClient::last_error() const {
  return {};
}

std::string FakeDaemonClient::endpoint() const {
  return config_.address + ":" + std::to_string(config_.port) + " (fake)";
}

bool FakeDaemonClient::get_version(std::string* version, std::string* error) {
  (void)error;
  if (version != nullptr) {
    // Honest about itself: a log line that cannot say whether the version came
    // from the daemon or from the simulation is worse than no version at all.
    *version = "fake";
  }
  return true;
}

bool FakeDaemonClient::get_config(json* config, std::string* error) {
  (void)error;
  if (config != nullptr) {
    *config = config_json_;
  }
  return true;
}

bool FakeDaemonClient::get_ptp_status(PtpStatus* status, std::string* error) {
  (void)error;
  if (status != nullptr) {
    *status = ptp_;
  }
  return true;
}

void FakeDaemonClient::set_ptp_status(const std::string& status) {
  ptp_.status = status;
}

size_t FakeDaemonClient::sink_count() const {
  return sinks_.size();
}

size_t FakeDaemonClient::source_count() const {
  return sources_.size();
}

bool FakeDaemonClient::get_sinks(json* sinks, std::string* error) {
  (void)error;
  if (sinks == nullptr) {
    return true;
  }
  json list = json::array();
  for (const auto& entry : sinks_) {
    json document = entry.second;
    document["id"] = entry.first;
    list.push_back(document);
  }
  *sinks = json{{"sinks", list}};
  return true;
}

bool FakeDaemonClient::get_sources(json* sources, std::string* error) {
  (void)error;
  if (sources == nullptr) {
    return true;
  }
  json list = json::array();
  for (const auto& entry : sources_) {
    json document = entry.second;
    document["id"] = entry.first;
    list.push_back(document);
  }
  *sources = json{{"sources", list}};
  return true;
}

bool FakeDaemonClient::put_sink(int id, const json& sink, std::string* error) {
  (void)error;
  sinks_[id] = sink;
  log().write(LogLevel::debug,
              "fake daemon: sink " + std::to_string(id) + " <- " + sink.dump());
  return true;
}

bool FakeDaemonClient::delete_sink(int id, std::string* error) {
  (void)error;
  sinks_.erase(id);
  return true;
}

bool FakeDaemonClient::put_source(int id, const json& source, std::string* error) {
  (void)error;
  sources_[id] = source;
  log().write(LogLevel::debug,
              "fake daemon: source " + std::to_string(id) + " <- " + source.dump());
  return true;
}

bool FakeDaemonClient::delete_source(int id, std::string* error) {
  (void)error;
  sources_.erase(id);
  return true;
}

bool FakeDaemonClient::get_sink_status(int id, SinkStatus* status,
                                       std::string* error) {
  (void)error;
  if (status == nullptr) {
    return true;
  }
  // "In use" means the daemon has a stream there, which for the fake is exactly
  // "somebody put one there".  A sink nobody wired reads as absent rather than
  // as silent, which is the distinction the real daemon makes too.
  *status = SinkStatus{};
  status->in_use = sinks_.find(id) != sinks_.end();
  status->receiving_rtp_packet = status->in_use;
  return true;
}

bool FakeDaemonClient::get_source_sdp(int id, std::string* sdp,
                                      std::string* error) {
  (void)error;
  if (sdp == nullptr) {
    return true;
  }
  const auto found = sources_.find(id);
  // No such source is an answer, not a failure — the same observable result the
  // HTTP client gives for the daemon's 404, so a caller cannot tell which
  // implementation it is talking to.
  *sdp = found == sources_.end() ? std::string() : source_sdp(id, found->second);
  return true;
}

std::string FakeDaemonClient::source_sdp(int id, const json& document) const {
  const std::string name = document.is_object() && document.contains("name") &&
                                   document["name"].is_string()
                               ? document["name"].get<std::string>()
                               : ("source " + std::to_string(id));
  const std::string codec = document.is_object() && document.contains("codec") &&
                                    document["codec"].is_string()
                                ? document["codec"].get<std::string>()
                                : "L24";
  int channels = 8;
  if (document.is_object() && document.contains("map") &&
      document["map"].is_array()) {
    channels = static_cast<int>(document["map"].size());
  }
  const unsigned ssrc = 0x50000000u + static_cast<unsigned>(id);
  return sdp_for(name, next_multicast(id), channels, ssrc, codec);
}

bool FakeDaemonClient::browse_sources(const std::string& kind, json* sources,
                                      std::string* error) {
  (void)error;
  if (sources == nullptr) {
    return true;
  }
  const std::string which = (kind == "mdns" || kind == "sap") ? kind : "all";

  // The two senders the measured network actually announces at 10.10.80.20
  // (docs/research/aes67-daemon-64ch.md).  Reproducing them rather than
  // inventing a fixture is the point of the fake: the eight-channel one is
  // exactly one of our blocks with the payload and packet time the
  // specification assumes, so a subscription built against it is a subscription
  // built against the real thing.
  json list = json::array();
  if (which != "mdns") {
    // Nothing is mDNS-announced on that bench, and a fake that answered every
    // kind with the same list would hide a mistake in which kind we asked for.
    list.push_back(json{
        {"source", "SAP"},
        {"id", "aes67-tx-1"},
        {"name", "AES67-TX-1"},
        {"domain", ""},
        {"address", "10.10.80.20"},
        {"sdp", sdp_for("AES67-TX-1", "233.254.57.0", 8, 0x1000u, "L24")},
        {"last_seen", 3},
        {"announce_period", 30},
    });
    list.push_back(json{
        {"source", "SAP"},
        {"id", "aes67-tx-2-qsys"},
        {"name", "AES67-TX-2-qsys"},
        {"domain", ""},
        {"address", "10.10.80.20"},
        {"sdp", sdp_for("AES67-TX-2-qsys", "239.1.0.99", 1, 0x1001u, "L24")},
        {"last_seen", 4},
        {"announce_period", 30},
    });
  }
  *sources = json{{"remote_sources", list}};
  return true;
}

}  // namespace aes67_srt::daemon
