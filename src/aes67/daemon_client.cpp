#include "aes67/daemon_client.hpp"

#include <sstream>

#include <httplib.h>

#include "aes67/fake_daemon_client.hpp"
#include "util.hpp"

namespace aes67_srt::daemon {
namespace {

using nlohmann::json;

bool fail(std::string* error, const std::string& message) {
  if (error != nullptr) {
    *error = message;
  }
  return false;
}

/** Human-readable description of a failed HTTP call, for the log and the UI. */
std::string describe(const httplib::Result& result) {
  if (!result) {
    return "no response from the AES67 daemon (" +
           httplib::to_string(result.error()) + ")";
  }
  std::ostringstream out;
  out << "AES67 daemon returned HTTP " << result->status;
  const std::string body = trim(result->body);
  if (!body.empty()) {
    // Truncated: a daemon answering with an HTML error page must not push a
    // page of markup into the log ring buffer and onto the status page.
    out << ": " << body.substr(0, 200);
  }
  return out.str();
}

/**
 * A document from a body that may not be one.
 *
 * An empty body is a normal answer to a PUT, and a body that will not parse is
 * the daemon telling us something we do not understand — either way there is no
 * document, and an empty object says so without throwing out of a status poll.
 */
json parse_or_empty(const std::string& body) {
  if (trim(body).empty()) {
    return json::object();
  }
  try {
    return json::parse(body);
  } catch (const json::exception&) {
    return json::object();
  }
}

/**
 * One member of a document, with a default.
 *
 * Absent, or present with the wrong type, gives the default: the daemon may add
 * and move fields, and a status poll is not the place to fail over one that is
 * only displayed.  Nothing read this way is used to make a safety decision.
 */
template <typename T>
T json_get(const json& document, const char* key, T fallback) {
  if (!document.is_object() || !document.contains(key)) {
    return fallback;
  }
  try {
    return document.at(key).get<T>();
  } catch (const json::exception&) {
    return fallback;
  }
}

}  // namespace

bool DaemonClient::stream_absent(int http_status) {
  return http_status == 400 || http_status == 404;
}

namespace {

/** The real thing: `aes67-daemon` over its REST API. */
class HttpDaemonClient : public DaemonClient {
 public:
  explicit HttpDaemonClient(const DaemonConfig& config)
      : client_(config.address, config.port),
        endpoint_(config.address + ":" + std::to_string(config.port)) {
    // Timeouts are short and deliberate.  This client is polled while an
    // operator watches a status page, so a daemon that has stopped answering
    // must read as "not reachable" in about a second rather than hanging the UI
    // for a TCP timeout measured in minutes.
    client_.set_connection_timeout(1, 0);
    client_.set_read_timeout(3, 0);
    client_.set_write_timeout(3, 0);
  }

  bool connected() const override { return connected_; }

  std::string last_error() const override { return last_error_; }

  std::string endpoint() const override { return endpoint_; }

  bool get_version(std::string* version, std::string* error) override {
    json document;
    if (!get("/api/version", &document, error)) {
      return false;
    }
    if (version != nullptr) {
      // The daemon has answered this as a bare string and as an object over its
      // history, and the version is only ever logged, so both are accepted
      // rather than one of them being treated as a failure.
      *version = document.is_string()
                     ? document.get<std::string>()
                     : json_get<std::string>(document, "version", "");
    }
    return true;
  }

  bool get_config(json* config, std::string* error) override {
    return get("/api/config", config, error);
  }

  bool get_ptp_status(PtpStatus* status, std::string* error) override {
    json document;
    if (!get("/api/ptp/status", &document, error)) {
      return false;
    }
    if (status != nullptr) {
      status->status = json_get<std::string>(document, "status", "unknown");
      status->gmid = json_get<std::string>(document, "gmid", "");
      status->jitter = json_get<int>(document, "jitter", 0);
    }
    return true;
  }

  bool get_sinks(json* sinks, std::string* error) override {
    return get("/api/sinks", sinks, error);
  }

  bool get_sources(json* sources, std::string* error) override {
    return get("/api/sources", sources, error);
  }

  bool put_sink(int id, const json& sink, std::string* error) override {
    return put("/api/sink/" + std::to_string(id), sink, error);
  }

  bool delete_sink(int id, std::string* error) override {
    return remove("/api/sink/" + std::to_string(id), error);
  }

  bool put_source(int id, const json& source, std::string* error) override {
    return put("/api/source/" + std::to_string(id), source, error);
  }

  bool delete_source(int id, std::string* error) override {
    return remove("/api/source/" + std::to_string(id), error);
  }

  bool get_sink_status(int id, SinkStatus* status, std::string* error) override {
    auto result = client_.Get("/api/sink/status/" + std::to_string(id));
    // A sink the daemon holds no stream for is normal, not a failure — see
    // stream_absent().  It is reported as in_use = false and the daemon stays
    // marked reachable, which is what a caller polling every block can act on.
    if (stream_absent(result ? result->status : 0)) {
      if (status != nullptr) {
        *status = SinkStatus{};
        status->in_use = false;
      }
      mark_connected();
      return true;
    }
    if (!accept(result, error)) {
      return false;
    }
    const json document = parse_or_empty(result->body);
    if (status != nullptr) {
      status->in_use = true;
      const json flags = json_get<json>(document, "sink_flags", json::object());
      status->receiving_rtp_packet =
          json_get<bool>(flags, "receiving_rtp_packet", false);
      status->muted = json_get<bool>(flags, "muted", false);
      status->rtp_seq_id_error = json_get<bool>(flags, "rtp_seq_id_error", false);
      status->rtp_ssrc_error = json_get<bool>(flags, "rtp_ssrc_error", false);
      status->rtp_payload_type_error =
          json_get<bool>(flags, "rtp_payload_type_error", false);
      status->rtp_sac_error = json_get<bool>(flags, "rtp_sac_error", false);
    }
    return true;
  }

  bool get_source_sdp(int id, std::string* sdp, std::string* error) override {
    auto result = client_.Get("/api/source/sdp/" + std::to_string(id));
    // Same rule as a sink's status: a source with no SDP is an answer, not a
    // failure, and an empty SDP is how the caller tells the difference.
    if (stream_absent(result ? result->status : 0)) {
      if (sdp != nullptr) {
        sdp->clear();
      }
      mark_connected();
      return true;
    }
    if (!accept(result, error)) {
      return false;
    }
    if (sdp != nullptr) {
      *sdp = result->body;
    }
    return true;
  }

  bool browse_sources(const std::string& kind, json* sources,
                      std::string* error) override {
    const std::string which = (kind == "mdns" || kind == "sap") ? kind : "all";
    return get("/api/browse/sources/" + which, sources, error);
  }

 private:
  void mark_connected() {
    connected_ = true;
    last_error_.clear();
  }

  bool mark_failed(const std::string& message, std::string* error) {
    connected_ = false;
    last_error_ = message;
    return fail(error, message);
  }

  /**
   * Any 2xx is success.
   *
   * The daemon answers 200 to the calls this project makes today; 204 is
   * accepted as well, so that a delete answering "no content" is not reported
   * as the daemon failing to answer a question it answered correctly.
   */
  bool accept(const httplib::Result& result, std::string* error) {
    if (!result || result->status < 200 || result->status > 299) {
      return mark_failed(describe(result), error);
    }
    mark_connected();
    return true;
  }

  bool get(const std::string& path, json* document, std::string* error) {
    auto result = client_.Get(path);
    if (!accept(result, error)) {
      return false;
    }
    if (document != nullptr) {
      *document = parse_or_empty(result->body);
    }
    return true;
  }

  bool put(const std::string& path, const json& document, std::string* error) {
    auto result = client_.Put(path, document.dump(), "application/json");
    return accept(result, error);
  }

  bool remove(const std::string& path, std::string* error) {
    auto result = client_.Delete(path);
    return accept(result, error);
  }

  httplib::Client client_;
  std::string endpoint_;
  bool connected_ = false;
  std::string last_error_;
};

}  // namespace

// ---------------------------------------------------------------------------
// The factory, and the documents this appliance asks the daemon to hold
// ---------------------------------------------------------------------------

std::unique_ptr<DaemonClient> DaemonClient::create(const DaemonConfig& config) {
  if (config.fake) {
    return std::unique_ptr<DaemonClient>(new FakeDaemonClient(config));
  }
  return std::unique_ptr<DaemonClient>(new HttpDaemonClient(config));
}

namespace {

/**
 * The daemon's word for the payload one block carries.
 *
 * `codec` is how the daemon names a stream's RTP payload, and these are the
 * values it takes for PCM (`docs/research/aes67-daemon-64ch.md`).  The
 * configuration validator has already refused every sample format but these
 * two, so the fallthrough is unreachable rather than a quiet default: a stream
 * whose declared payload disagreed with the samples handed to it would be the
 * wrong audio, with nothing anywhere reporting it.
 */
std::string codec_for(const std::string& format) {
  return format == "s16_le" ? "L16" : "L24";
}

/**
 * The RTP payload type the daemon expects for a codec.
 *
 * `98` is not a guess: it is the value in the daemon's own source template
 * (`daemon/json.cpp` at `json_to_source`), paired there with `codec: "L24"`. The
 * sibling `aes67-sip` sends the same number for the same reason — it read the
 * template, which is why its values are the daemon's defaults rather than an
 * invented policy.
 *
 * `97` for L16 is AES67's static L16 payload type and is **the one value here the
 * template does not confirm**, because its example is L24. v1 ships L24, and the
 * L16 case is recorded as unverified in `docs/research/aes67-daemon-64ch.md`.
 */
uint8_t payload_type_for(const std::string& format) {
  return format == "s16_le" ? 97 : 98;
}

}  // namespace

std::string block_stream_name(int block_index) {
  return "aes67-srt block " + std::to_string(block_index);
}

json make_block_source(const Config& config, const BlockConfig& block) {
  // Every field here is one the daemon *reads*, and it reads them all with
  // `pt.get<T>(...)`, which throws when a node is missing. The first version of
  // this document omitted `ttl`, `payload_type`, `dscp` and
  // `refclk_ptp_traceable`, on the reasoning that inventing a site's multicast
  // policy was worse than omitting a field — and the daemon answered with
  // `HTTP 400: error parsing JSON: No such node (ttl)` on a Pi. Omitting was
  // never the safe option; reading the daemon's own schema was.
  //
  // The values are the daemon's own defaults, from the template in
  // `daemon/json.cpp` at `json_to_source` (bondagit-4.0.1, commit 68bd278), so
  // this is what the daemon would produce for a source created in its own web UI.
  // TTL and DSCP are arguably site policy — multicast scope and QoS marking — and
  // making them configurable is an open item rather than a decision taken here.
  return json{
      // Always enabled, even for a muted block: mute is applied in this
      // appliance's own pipeline, and a stream that vanishes from the far end's
      // routing grid because somebody pressed mute is a call-out, not a mute.
      {"enabled", true},
      // From the index alone, never from the channels: an operator who re-maps
      // which device channels a block carries must not thereby rename the
      // stream that the far end has already subscribed to.
      {"name", block_stream_name(block.index)},
      {"io", "Audio Device"},
      {"codec", codec_for(config.audio.format)},
      {"address", ""},  // empty: let the daemon choose from its multicast base
      {"max_samples_per_packet", config.audio.period_frames},
      {"map", block.channels},
      {"ttl", 15},
      {"payload_type", payload_type_for(config.audio.format)},
      {"dscp", 34},
      {"refclk_ptp_traceable", false},
  };
}

/**
 * The sink's playout delay, in samples.
 *
 * 384 is the value in the daemon's own sink template (`daemon/json.cpp` at
 * `json_to_sink`), so this is what the daemon gives a sink created in its own web
 * UI — 8 ms at 48 kHz, which is also eight of our 1 ms periods.
 *
 * **Why it is not zero, which is what we sent first.** The reasoning was that the
 * daemon should add no delay because the A/V delay line is ours
 * (`egress.delay_ms`), and that mistook the mechanism: this is not a competing
 * delay line, it is the sink's *receive buffer* — the thing that absorbs network
 * jitter and aligns the RTP timeline with the ALSA one. Zero means no buffer at
 * all, which is unusable even where a driver accepts it.
 *
 * The 2026-09-17 session saw the driver refuse our sink with
 * `failed to add sink 0 : (driver) command failed`, and the working theory is that
 * this zero is what it objected to. **That is a hypothesis, not a measurement**:
 * the next commissioning run is what tests it, and what it will show is either a
 * sink that subscribes or a refusal that names something else.
 */
constexpr int k_sink_playout_delay_samples = 384;

bool make_block_sink(const BlockConfig& block, const std::string& remote_sdp,
                     json* sink, std::string* error) {
  if (sink == nullptr) {
    return fail(error, "internal error: sink document pointer is null");
  }
  if (trim(remote_sdp).empty()) {
    return fail(error,
                "block " + std::to_string(block.index) +
                    " has no remote SDP: pick a discovered SAP/mDNS source, or "
                    "paste the SDP of the stream it should subscribe to");
  }

  *sink = json{
      {"name", block_stream_name(block.index)},
      {"io", "Audio Device"},
      // The daemon adds no delay of its own. The A/V delay line is ours
      // (config egress.delay_ms), so there is exactly one place in the path
      // where a sample can be held back, and it is the one the operator
      // controls.
      // The sink's receive buffer, not a competing delay line — see the note on
      // k_sink_playout_delay_samples. Zero is refused by the driver.
      {"delay", k_sink_playout_delay_samples},
      // Subscribe to the SDP below rather than to a `source` URL the daemon
      // would have to fetch.
      {"use_sdp", true},
      {"source", ""},
      {"sdp", remote_sdp},
      // False: refuse a stream whose reference clock is not the grandmaster
      // this device is locked to. That is the safe default both ways round —
      // a stream from another clock domain would play at the wrong rate and
      // sound subtly wrong rather than obviously broken.
      {"ignore_refclk_gmid", false},
      {"map", block.channels},
  };
  return true;
}

}  // namespace aes67_srt::daemon
