#include "http/api_server.hpp"

#include <sys/stat.h>

#include <cstdlib>
#include <fstream>
#include <sstream>

#include <httplib.h>

#include "aes67/daemon_client.hpp"
#include "engine.hpp"
#include "log.hpp"
#include "version.hpp"

namespace aes67_srt {
namespace {

bool directory_exists(const std::string& path) {
  if (path.empty()) {
    return false;
  }
  struct stat info {};
  if (stat(path.c_str(), &info) != 0) {
    return false;
  }
  return (info.st_mode & S_IFDIR) != 0;
}

void reply_json(httplib::Response& response, const nlohmann::json& body,
                int status = 200) {
  response.status = status;
  response.set_content(body.dump(), "application/json");
}

void reply_text(httplib::Response& response, int status,
                const std::string& message) {
  response.status = status;
  response.set_content(message, "text/plain");
}

/**
 * The page served until the built web UI exists.
 *
 * It is not a placeholder that says "not implemented": it polls `/api/status` and
 * renders exactly the same preflight the real page will, so an appliance with no
 * daemon and no link still loads a page and says what is wrong — which is one of
 * this ticket's acceptance criteria, and a behaviour worth being able to exercise
 * before any front-end toolchain is involved.
 */
const char* k_fallback_page = R"HTML(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>aes67-srt</title>
<style>
 body { font: 15px/1.5 system-ui, sans-serif; margin: 2rem; max-width: 46rem; color: #111; }
 h1 { font-size: 1.25rem; margin: 0 0 .25rem; }
 .sub { color: #666; margin: 0 0 1.5rem; }
 table { border-collapse: collapse; width: 100%; margin-bottom: 1.5rem; }
 th, td { text-align: left; padding: .4rem .6rem; border-bottom: 1px solid #ddd; vertical-align: top; }
 .ok { color: #0a7d28; } .bad { color: #b00020; font-weight: 600; }
 .num { font-variant-numeric: tabular-nums; }
 code { background: #f4f4f4; padding: .1rem .3rem; border-radius: 3px; }
</style>
</head>
<body>
<h1>aes67-srt</h1>
<p class="sub" id="version">loading…</p>
<div id="root">loading…</div>
<script>
async function poll() {
  try {
    const r = await fetch('/api/status');
    const s = await r.json();
    document.getElementById('version').textContent =
      s.name + ' ' + s.version + ' (' + s.build + '), ' + s.role + ' ' + s.mode +
      (s.peer ? ' to ' + s.peer : '');
    const p = s.preflight || { checks: [] };
    let html = '<table><tr><th>check</th><th>state</th><th>detail</th></tr>';
    for (const c of p.checks) {
      html += '<tr><td>' + c.name + '</td><td class="' + (c.ok ? 'ok' : 'bad') + '">' +
        (c.ok ? 'ok' : 'FAIL') + '</td><td>' + c.detail + '</td></tr>';
    }
    html += '</table>';
    html += '<table><tr><th>figure</th><th>value</th></tr>' +
      '<tr><td>playout delay</td><td class="num">' + s.engine.delay_ms.toFixed(1) + ' ms</td></tr>' +
      '<tr><td>A/V offset</td><td class="num">' + s.engine.egress_delay_ms.toFixed(1) + ' ms</td></tr>' +
      '<tr><td>clock correction</td><td class="num">' + s.engine.clock_offset_ppm.toFixed(2) + ' ppm</td></tr>' +
      '<tr><td>frames sent / received / refused</td><td class="num">' +
        s.engine.frames_sent + ' / ' + s.engine.frames_received + ' / ' +
        s.engine.frames_refused + '</td></tr></table>';
    if (!p.ok) {
      html = '<p class="bad">Preflight failed: audio will not flow until the checks below pass.</p>' + html;
    }
    document.getElementById('root').innerHTML = html;
  } catch (e) {
    document.getElementById('root').textContent = 'cannot reach the appliance: ' + e;
  }
}
poll();
setInterval(poll, 1000);
</script>
</body>
</html>
)HTML";

}  // namespace

ApiServer::ApiServer(Config* config, Engine* engine, daemon::DaemonClient* daemon,
                     std::string webui_dir)
    : config_(config),
      engine_(engine),
      daemon_(daemon),
      webui_dir_(std::move(webui_dir)) {}

ApiServer::~ApiServer() {
  stop();
}

nlohmann::json ApiServer::build_preflight() {
  const EngineStatus status =
      engine_ != nullptr ? engine_->status() : EngineStatus{};

  daemon::PtpStatus ptp;
  std::string daemon_error;
  bool daemon_ok = false;
  if (daemon_ != nullptr) {
    std::lock_guard<std::mutex> lock(daemon_mutex_);
    daemon_ok = daemon_->get_ptp_status(&ptp, &daemon_error);
  } else {
    daemon_error = "no daemon client";
  }

  const bool ptp_locked = daemon_ok && ptp.status == "locked";
  const bool device_open = engine_ != nullptr && engine_->backend() != nullptr &&
                           engine_->backend()->is_open();
  const bool link_open = status.link_open;

  nlohmann::json checks = nlohmann::json::array();
  // PTP first: an unlocked slave is the failure that looks like success.
  checks.push_back(
      {{"name", "ptp"},
       {"ok", ptp_locked},
       {"detail", !daemon_ok ? std::string("unknown: the daemon is unreachable")
                             : (ptp_locked ? std::string("locked")
                                           : "not locked: " + ptp.status)}});
  checks.push_back({{"name", "daemon"},
                    {"ok", daemon_ok},
                    {"detail", daemon_ok ? daemon_->endpoint() : daemon_error}});
  checks.push_back({{"name", "device"},
                    {"ok", device_open},
                    {"detail", engine_ != nullptr && engine_->backend() != nullptr
                                   ? engine_->backend()->detail()
                                   : std::string("no audio backend")}});
  checks.push_back(
      {{"name", "link"},
       {"ok", link_open},
       {"detail", link_open ? (config_ != nullptr
                                   ? config_->link.mode + " " + config_->link.role
                                   : std::string("open"))
                            : std::string("not open")}});

  const bool ok = ptp_locked && daemon_ok && device_open && link_open;
  return {{"ok", ok}, {"delay_ms", status.delay_ms}, {"checks", checks}};
}

nlohmann::json ApiServer::build_status() {
  const EngineStatus status =
      engine_ != nullptr ? engine_->status() : EngineStatus{};

  nlohmann::json engine;
  engine["running"] = status.running;
  engine["link_open"] = status.link_open;
  engine["frames_sent"] = status.frames_sent;
  engine["frames_received"] = status.frames_received;
  engine["frames_refused"] = status.frames_refused;
  engine["silence_periods"] = status.silence_periods;
  engine["delay_ms"] = status.delay_ms;
  engine["delay_fraction"] = status.delay_fraction;
  engine["egress_delay_ms"] = status.egress_delay_ms;
  engine["av_delay_ms"] = status.av_delay_ms;
  engine["clock_offset_ppm"] = status.clock_offset_ppm;
  engine["clock_ratio"] = status.clock_ratio;
  engine["test_signal_channel"] = status.test_signal_channel;

  nlohmann::json link;
  link["available"] = status.link_stats_available;
  if (status.link_stats_available) {
    const transport::LinkStats& stats = status.link_stats;
    link["rtt_ms"] = stats.rtt_ms;
    link["bandwidth_mbps"] = stats.bandwidth_mbps;
    link["receive_rate_mbps"] = stats.receive_rate_mbps;
    link["receive_buffer_ms"] = stats.receive_buffer_ms;
    link["negotiated_latency_ms"] = stats.negotiated_latency_ms;
    link["send_buffer_ms"] = stats.send_buffer_ms;
    link["packets_received"] = stats.packets_received;
    link["packets_lost"] = stats.packets_lost;
    link["packets_retransmitted"] = stats.packets_retransmitted;
    link["packets_dropped"] = stats.packets_dropped;
  }

  nlohmann::json document;
  document["name"] = "aes67-srt";
  document["version"] = version_string();
  document["build"] = build_info();
  if (config_ != nullptr) {
    document["role"] = config_->link.role;
    document["mode"] = config_->link.mode;
    document["peer"] = config_->link.peer;
    document["blocks"] = config_->blocks.size();
    document["channels"] = config_->audio.channels;
  }
  document["preflight"] = build_preflight();
  document["engine"] = engine;
  document["link"] = link;
  return document;
}

void ApiServer::register_routes() {
  httplib::Server* svr = server_.get();

  svr->Get("/api/version",
           [](const httplib::Request&, httplib::Response& response) {
             reply_json(response, {{"name", "aes67-srt"},
                                   {"version", version_string()},
                                   {"build", build_info()}});
           });

  svr->Get("/api/status",
           [this](const httplib::Request&, httplib::Response& response) {
             reply_json(response, build_status());
           });

  svr->Get(
      "/api/log", [](const httplib::Request& request, httplib::Response& response) {
        int lines = 200;
        if (request.has_param("lines")) {
          lines = std::atoi(request.get_param_value("lines").c_str());
          if (lines <= 0 || lines > 500) {
            lines = 500;
          }
        }
        reply_json(response, {{"lines", log().tail(static_cast<size_t>(lines))}});
      });

  // ---- the UI ------------------------------------------------------------
  // The built front end wins when it is there; otherwise the built-in page still
  // loads and says what is wrong. Either way the appliance serves its own UI.
  if (directory_exists(webui_dir_)) {
    svr->set_mount_point("/", webui_dir_);
    const std::string index = webui_dir_ + "/index.html";
    svr->Get("/(.*)",
             [index](const httplib::Request& request, httplib::Response& response) {
               if (request.path.rfind("/api/", 0) == 0) {
                 reply_text(response, 404, "unknown API endpoint " + request.path);
                 return;
               }
               std::ifstream input(index, std::ios::binary);
               if (!input.is_open()) {
                 reply_text(response, 500, "cannot read " + index);
                 return;
               }
               std::stringstream buffer;
               buffer << input.rdbuf();
               response.status = 200;
               response.set_content(buffer.str(), "text/html");
             });
    log().write(LogLevel::info, "control: serving the web UI from " + webui_dir_);
  } else {
    svr->Get("/(.*)",
             [](const httplib::Request& request, httplib::Response& response) {
               if (request.path.rfind("/api/", 0) == 0) {
                 reply_text(response, 404, "unknown API endpoint " + request.path);
                 return;
               }
               response.status = 200;
               response.set_content(k_fallback_page, "text/html");
             });
    log().write(LogLevel::info,
                "control: no web UI built (" + webui_dir_ +
                    " is absent); serving the built-in status page");
  }

  svr->set_error_handler([](const httplib::Request&, httplib::Response& response) {
    if (response.status == 404 && response.body.empty()) {
      reply_text(response, 404, "not found");
    }
  });

  svr->set_exception_handler([](const httplib::Request& request,
                                httplib::Response& response,
                                std::exception_ptr error) {
    std::string message = "internal error";
    try {
      std::rethrow_exception(error);
    } catch (const std::exception& ex) {
      message = std::string("internal error: ") + ex.what();
    } catch (...) {
      message = "internal error: unknown exception";
    }
    log().write(LogLevel::error,
                "control: " + request.method + " " + request.path + ": " + message);
    reply_text(response, 500, message);
  });
}

bool ApiServer::start(std::string* error) {
  if (running_) {
    return true;
  }
  server_ = std::make_unique<httplib::Server>();
  register_routes();

  const std::string address = (config_ != nullptr && !config_->http_addr.empty())
                                  ? config_->http_addr
                                  : "0.0.0.0";
  const int port = (config_ != nullptr) ? config_->http_port : 8082;
  if (!server_->bind_to_port(address, port)) {
    if (error != nullptr) {
      *error = "control: cannot bind " + address + ":" + std::to_string(port) +
               " (is another instance already running?)";
    }
    server_.reset();
    return false;
  }

  running_ = true;
  thread_ = std::thread([this, address, port] {
    log().write(LogLevel::info, "control: REST API listening on http://" + address +
                                    ":" + std::to_string(port));
    server_->listen_after_bind();
    running_ = false;
  });
  return true;
}

void ApiServer::stop() {
  if (!running_ && !thread_.joinable()) {
    return;
  }
  if (server_) {
    server_->stop();
  }
  if (thread_.joinable()) {
    thread_.join();
  }
  server_.reset();
  running_ = false;
}

}  // namespace aes67_srt
