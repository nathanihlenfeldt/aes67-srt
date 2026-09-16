#pragma once

#include <map>
#include <string>

#include "aes67/daemon_client.hpp"

namespace aes67_srt::daemon {

/**
 * A daemon with no hardware behind it.
 *
 * It exists so that the whole commissioning path — subscribe, subscribe to our
 * own source, read PTP state, preflight — can be exercised on a machine with no
 * RAVENNA kernel module, no daemon and no network.  That is ticket 09's second
 * acceptance criterion, and it is the reason this class is a device with
 * defined behaviour rather than a mock that returns whatever a test wants.
 *
 * **Its answers are shaped by a real one.** The configuration it reports, the
 * two senders discovery returns and the PTP state are what a provisioned Pi 5
 * actually reported (`docs/research/aes67-daemon-64ch.md`, issue #18), so code
 * written against the fake is written against the shapes the real daemon gives
 * — not against an invented API that happens to satisfy our own tests.
 *
 * Two places it is deliberately *looser* than the daemon:
 *
 *  - it starts with no sinks and no sources, exactly as a fresh install does,
 *    and reports `in_use = false` / an empty SDP for a stream it does not hold
 *    — the same observable answer the HTTP client gives to the daemon's 400 or
 *    404, so both implementations are indistinguishable to a caller;
 *  - nothing reaches the wire.  `put_source` makes a stream that other code can
 *    read back and subscribe to; it does not put audio on the network.
 */
class FakeDaemonClient : public DaemonClient {
 public:
  explicit FakeDaemonClient(const DaemonConfig& config);

  bool connected() const override;
  std::string last_error() const override;
  std::string endpoint() const override;

  bool get_version(std::string* version, std::string* error) override;
  bool get_config(json* config, std::string* error) override;
  bool get_ptp_status(PtpStatus* status, std::string* error) override;

  bool get_sinks(json* sinks, std::string* error) override;
  bool get_sources(json* sources, std::string* error) override;

  bool put_sink(int id, const json& sink, std::string* error) override;
  bool delete_sink(int id, std::string* error) override;
  bool put_source(int id, const json& source, std::string* error) override;
  bool delete_source(int id, std::string* error) override;

  bool get_sink_status(int id, SinkStatus* status, std::string* error) override;
  bool get_source_sdp(int id, std::string* sdp, std::string* error) override;

  bool browse_sources(const std::string& kind, json* sources,
                      std::string* error) override;

  /**
   * Make it report a PTP state other than `locked`.
   *
   * Not a convenience: unlocked PTP is the commonest way an appliance produces
   * no audio while looking perfectly healthy, so the preflight's job is to
   * catch it — and a fake that can only ever be locked would leave that check
   * unprovable in CI.  A preflight test that cannot fail is not a test.
   */
  void set_ptp_status(const std::string& status);

  /** The sinks and sources it holds, by index, as a caller would read them. */
  size_t sink_count() const;
  size_t source_count() const;

 private:
  /** The SDP of a source it holds: derived from the document, as the daemon's would
   * be. */
  std::string source_sdp(int id, const json& document) const;

  DaemonConfig config_;
  json config_json_;
  PtpStatus ptp_;
  std::map<int, json> sinks_;
  std::map<int, json> sources_;
};

}  // namespace aes67_srt::daemon
