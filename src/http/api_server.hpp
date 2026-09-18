#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "config.hpp"

namespace httplib {
class Server;
}  // namespace httplib

namespace aes67_srt {

class Engine;
class Service;

namespace daemon {
class DaemonClient;
}  // namespace daemon

/**
 * The control surface: the polling REST API and the host for the web UI.
 *
 * Ticket 13 (issue #14) is the first slice, and this is the whole of it: one
 * status document that carries the preflight checks and the live figures, polled
 * by a page the appliance serves itself. It is deliberately a *facade* — it owns
 * no audio state and steers nothing yet — because the alternative is a second
 * copy of the engine's truth that drifts from it.
 *
 * **Preflight leads with PTP**, because an unlocked slave is the most common way
 * this appliance looks healthy while producing silence: the daemon answers, the
 * device opens, the link is up, and no audio flows. It is reported as a failure,
 * not a warning, and the status document says `ok: false` for the whole preflight
 * when it is unlocked.
 *
 * **The daemon probe is the reachability check.** One `get_ptp_status` is both
 * "is the daemon there" and "is PTP locked"; a failure there is reported as the
 * daemon being unreachable, and PTP as unknown rather than guessed.
 *
 * The daemon client is used from HTTP worker threads while the appliance runs, so
 * every call here is serialised behind one lock. It is never on the audio path,
 * so a slow daemon can delay a status poll and nothing else.
 */
class ApiServer {
 public:
  ApiServer(Config* config, Service* service, daemon::DaemonClient* daemon,
            std::string webui_dir, std::string config_path);
  ~ApiServer();

  ApiServer(const ApiServer&) = delete;
  ApiServer& operator=(const ApiServer&) = delete;

  /** Bind and serve. Refuses with a reason when the port is already taken. */
  bool start(std::string* error);
  void stop();

  bool running() const { return running_; }
  int port() const { return config_ != nullptr ? config_->http_port : 0; }

  /** The whole `/api/status` document. Public so a test can assert it directly. */
  nlohmann::json build_status();

  /** The preflight block alone: the checks that decide whether audio can flow. */
  nlohmann::json build_preflight();

 private:
  void register_routes();

  /** A copy of the running configuration, taken under the lock. */
  Config config_snapshot();

  /** The engine, or null when it is stopped. */
  Engine* engine();

  Config* config_{nullptr};
  Service* service_{nullptr};
  daemon::DaemonClient* daemon_{nullptr};
  std::string webui_dir_;
  /** Where a configuration POST is written. Empty means "do not persist". */
  std::string config_path_;
  /**
   * Fields changed by a POST that only take effect on a restart. Kept so the page
   * can say so instead of the change appearing to have landed.
   */
  std::vector<std::string> pending_restart_;
  /** Guards `config_` and `pending_restart_`: HTTP worker threads share them. */
  std::mutex config_mutex_;
  /** Serialises the daemon, which is not safe to drive from two HTTP threads. */
  std::mutex daemon_mutex_;
  std::unique_ptr<httplib::Server> server_;
  std::thread thread_;
  bool running_{false};
};

}  // namespace aes67_srt
