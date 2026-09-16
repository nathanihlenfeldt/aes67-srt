#pragma once

#include <memory>
#include <string>

#include <nlohmann/json.hpp>

#include "config.hpp"

/**
 * The `aes67-daemon` REST client.
 *
 * The daemon is a black box this project reaches over HTTP and never forks,
 * vendors or patches (spec decision 11).  It owns discovery, subscription, SDP
 * and PTP; this module owns the conversation with it and nothing else.  The
 * audio device the daemon exposes is a different module — see
 * `audio/backend.hpp` — which is why nothing here knows what a sample is.
 *
 * Two implementations of one interface:
 *
 *  - `daemon::HttpDaemonClient`, talking to a real daemon;
 *  - `daemon::FakeDaemonClient`, an in-process daemon whose answers are shaped
 *    by what a provisioned Pi 5 actually reports
 *    (`docs/research/aes67-daemon-64ch.md`).
 *
 * The fake is not a mock.  It is a daemon with defined behaviour and no
 * hardware, and it is what lets this module, the preflight and the commissioning
 * loopback be tested in CI on a machine that has neither a daemon nor a Pi
 * (ticket 09's second acceptance criterion).
 *
 * Implementations are used from one thread at a time, as the audio backend is.
 */
namespace aes67_srt::daemon {

/**
 * Whole daemon documents are handled as JSON.
 *
 * This is the one place the project puts a third-party type in a public
 * interface, and it is deliberate: these documents belong to the daemon's API,
 * not to this project.  We read a handful of fields and hand the rest back
 * unchanged, so a typed mirror would be a copy of an API we do not own, with a
 * maintenance bill every time the daemon moves.  The two things this project
 * genuinely *interprets* — PTP state and sink status — are typed structs.
 */
using json = nlohmann::json;

/** PTP slave state, as `GET /api/ptp/status` reports it. */
struct PtpStatus {
  /** `unlocked`, `locking` or `locked`. Until it is `locked`, no audio flows. */
  std::string status{"unknown"};
  /** Grandmaster clock identity, e.g. `6C-DF-FB-FF-FE-01-92-5C`. */
  std::string gmid;
  int jitter{0};
};

/** Per-sink RTP reception flags, as `GET /api/sink/status/N` reports them. */
struct SinkStatus {
  /**
   * False when the daemon has no stream on this sink at all — nothing
   * subscribed there, nothing configured there.  That is a normal state on a
   * sink that has not been wired yet, and it is the difference between "nothing
   * is arriving" and "there is nothing for anything to arrive on".
   */
  bool in_use{true};
  bool receiving_rtp_packet{false};
  bool muted{false};
  /** The four ways a stream can exist and still not be carrying our audio. */
  bool rtp_seq_id_error{false};
  bool rtp_ssrc_error{false};
  bool rtp_payload_type_error{false};
  bool rtp_sac_error{false};
};

class DaemonClient {
 public:
  virtual ~DaemonClient() = default;

  /** True when the daemon answered the last request. */
  virtual bool connected() const = 0;

  /** The last transport or HTTP failure, empty when healthy. */
  virtual std::string last_error() const = 0;

  /** The daemon this client talks to, for the log and the status page. */
  virtual std::string endpoint() const = 0;

  /** `GET /api/version`. The daemon's version, which the Pi research asked for. */
  virtual bool get_version(std::string* version, std::string* error) = 0;

  /**
   * `GET /api/config`.
   *
   * Read because two of its settings are load-bearing rather than cosmetic
   * (`docs/research/aes67-daemon-64ch.md`): an `interface_name` of `lo` never
   * sees PTP or RTP, and `auto_sinks_update` can retarget a sink this appliance
   * wired deliberately.  Both look like mysterious breakage when they are wrong,
   * and neither is visible from anywhere else.
   *
   * There is deliberately no `set_config`: the daemon's configuration belongs
   * to provisioning (`scripts/install-daemon.sh`), and an appliance that is
   * meant to treat the daemon as a black box should not be rewriting it.
   */
  virtual bool get_config(json* config, std::string* error) = 0;

  virtual bool get_ptp_status(PtpStatus* status, std::string* error) = 0;

  /** Raw `GET /api/sinks` document: every sink the daemon holds. */
  virtual bool get_sinks(json* sinks, std::string* error) = 0;
  /** Raw `GET /api/sources` document. */
  virtual bool get_sources(json* sources, std::string* error) = 0;

  /** `PUT /api/sink/N`. Creates or replaces the stream at that index. */
  virtual bool put_sink(int id, const json& sink, std::string* error) = 0;
  virtual bool delete_sink(int id, std::string* error) = 0;
  virtual bool put_source(int id, const json& source, std::string* error) = 0;
  virtual bool delete_source(int id, std::string* error) = 0;

  virtual bool get_sink_status(int id, SinkStatus* status, std::string* error) = 0;

  /**
   * The SDP of one of the daemon's own sources, `GET /api/source/sdp/N`.
   *
   * This is how our sources are handed to somebody else's equipment, and how
   * the commissioning loopback wires a sink to a source on the same box.
   */
  virtual bool get_source_sdp(int id, std::string* sdp, std::string* error) = 0;

  /** `kind` is `all`, `mdns` or `sap`; anything else is read as `all`. */
  virtual bool browse_sources(const std::string& kind, json* sources,
                              std::string* error) = 0;

  /**
   * True when a response means "the daemon has no stream there" rather than
   * "something failed".
   *
   * The daemon answers 400 or 404 on its per-stream paths for a sink or source
   * with no stream yet.  That is a normal state — a sink nobody has subscribed
   * to, an SDP not configured — and it must not be reported as the daemon being
   * unreachable, which is what a status poll would otherwise flash on every
   * poll, for every block that is not wired yet.
   */
  static bool stream_absent(int http_status);

  /** Builds the client the configuration asks for. */
  static std::unique_ptr<DaemonClient> create(const DaemonConfig& config);
};

/**
 * The name of the daemon stream carrying one block.
 *
 * A name is not decoration here: it is what a human reads in the daemon's
 * routing grid, or in another vendor's controller, to know which of eight
 * identical-looking streams is which.  It is derived from the block index and
 * never parsed back, so it only has to be recognisable — but it must be, or the
 * eight streams of a 64-channel link are indistinguishable from each other.
 */
std::string block_stream_name(int block_index);

/**
 * The daemon document for one block's *source*: the stream this appliance
 * publishes, carrying the block's eight device channels.
 *
 * The channel mapping is the `map` field, and it is the whole of the
 * block-to-device mapping on this side — data, taken from
 * `BlockConfig::channels`, not derived from anything.  `codec` follows the
 * configured sample format, because a stream whose daemon payload disagrees
 * with the samples handed to it is not an error anywhere: it is quietly the
 * wrong audio.
 */
json make_block_source(const Config& config, const BlockConfig& block);

/**
 * The daemon document for one block's *sink*: the remote stream this appliance
 * subscribes to, landing on the block's eight device channels.
 *
 * A sink describes somebody else's RTP stream, so it needs that stream's SDP —
 * discovered through `browse_sources`, or pasted in by hand.  Without one there
 * is nothing to subscribe to, so this refuses and says which block and why,
 * rather than putting a sink on the daemon that can never receive.
 */
bool make_block_sink(const BlockConfig& block, const std::string& remote_sdp,
                     json* sink, std::string* error);

}  // namespace aes67_srt::daemon
