#include "http/api_server.hpp"

#include <sys/stat.h>

#include <cstdio>
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

/** Parse a JSON request body, setting |error| rather than throwing. */
nlohmann::json parse_body(const httplib::Request& request, std::string* error) {
  if (request.body.empty()) {
    *error = "empty request body";
    return nlohmann::json::object();
  }
  try {
    return nlohmann::json::parse(request.body);
  } catch (const std::exception& ex) {
    *error = std::string("invalid JSON: ") + ex.what();
    return nlohmann::json::object();
  }
}

/**
 * Write a configuration file by replace, not by truncate.
 *
 * A half-written file is an appliance that will not start, so the new document
 * goes to a sibling and a rename (atomic within a filesystem) puts it in place.
 */
bool write_atomically(const std::string& path, const std::string& text,
                      std::string* error) {
  const std::string temporary = path + ".tmp";
  {
    std::ofstream out(temporary, std::ios::trunc);
    if (!out.is_open()) {
      *error = "cannot write " + temporary;
      return false;
    }
    out << text;
    if (!out.good()) {
      *error = "error writing " + temporary;
      return false;
    }
  }
  if (std::rename(temporary.c_str(), path.c_str()) != 0) {
    *error = "cannot replace " + path;
    return false;
  }
  return true;
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
 .controls { margin: 1rem 0 2rem; display: flex; gap: .5rem; align-items: center; flex-wrap: wrap; }
 .controls input { width: 6rem; padding: .2rem .3rem; }
 #controlmsg { color: #666; }
 h2 { font-size: 1rem; margin: 1.5rem 0 .5rem; }
 .config label { display: inline-block; margin: 0 1rem .6rem 0; }
 .config input, .config select { padding: .15rem .3rem; }
 #configmsg, #controlmsg { color: #666; }
 code { background: #f4f4f4; padding: .1rem .3rem; border-radius: 3px; }
</style>
</head>
<body>
<h1>aes67-srt</h1>
<p class="sub" id="version">loading…</p>
<div id="root">loading…</div>
<div class="controls">
  <label>A/V delay (ms): <input id="delay" type="number" min="0" max="5000" step="0.1"></label>
  <button onclick="setDelay()">Set</button>
  <button onclick="triggerTestSignal()">Trigger test signal</button>
  <span id="controlmsg"></span>
</div>
<h2>SRT link</h2>
<form class="config" onsubmit="return false;">
  <label>mode <select id="c_mode"><option>caller</option><option>listener</option><option>rendezvous</option><option>loopback</option></select></label>
  <label>role <select id="c_role"><option>tx</option><option>rx</option><option>duplex</option></select></label>
  <label>peer <input id="c_peer" size="16" placeholder="host:port"></label>
  <label>local port <input id="c_local_port" type="number" size="6"></label>
  <label>latency (ms) <input id="c_latency_ms" type="number" size="6"></label>
  <label>blocks <input id="c_blocks" type="number" min="1" max="8" size="2"></label>
  <label>passphrase <input id="c_passphrase" type="password" size="12"></label>
  <button onclick="saveLink()">Save SRT settings</button>
  <span id="configmsg"></span>
</form>
<script>
let currentConfig = null;
async function loadConfig() {
  try {
    const r = await fetch('/api/config');
    currentConfig = await r.json();
    const l = currentConfig.link;
    document.getElementById('c_mode').value = l.mode;
    document.getElementById('c_role').value = l.role;
    document.getElementById('c_peer').value = l.peer || '';
    document.getElementById('c_local_port').value = l.local_port;
    document.getElementById('c_latency_ms').value = l.latency_ms;
    document.getElementById('c_blocks').value = l.blocks;
    document.getElementById('c_passphrase').value = l.passphrase || '';
  } catch (e) { /* the status poll already reports an unreachable appliance */ }
}
async function saveLink() {
  if (!currentConfig) { await loadConfig(); }
  if (!currentConfig) return;
  const num = (id) => parseInt(document.getElementById(id).value, 10);
  currentConfig.link.mode = document.getElementById('c_mode').value;
  currentConfig.link.role = document.getElementById('c_role').value;
  currentConfig.link.peer = document.getElementById('c_peer').value;
  currentConfig.link.local_port = num('c_local_port');
  currentConfig.link.latency_ms = num('c_latency_ms');
  currentConfig.link.blocks = num('c_blocks');
  currentConfig.link.passphrase = document.getElementById('c_passphrase').value;
  const msg = document.getElementById('configmsg');
  const r = await fetch('/api/config', {
    method: 'POST', headers: {'Content-Type': 'application/json'},
    body: JSON.stringify(currentConfig)
  });
  if (!r.ok) {
    msg.className = 'bad';
    msg.textContent = await r.text();
    return;
  }
  const res = await r.json();
  const restart = res.restart_required || [];
  msg.className = restart.length ? 'bad' : 'ok';
  msg.textContent = restart.length
    ? 'saved to ' + res.path + ' -- restart the appliance to apply: ' + restart.join(', ')
    : 'saved to ' + res.path;
}
async function setDelay() {
  const value = parseFloat(document.getElementById('delay').value);
  const r = await fetch('/api/egress/delay', {
    method: 'POST', headers: {'Content-Type': 'application/json'},
    body: JSON.stringify({delay_ms: value})
  });
  document.getElementById('controlmsg').textContent =
    r.ok ? 'A/V delay set to ' + value + ' ms' : await r.text();
}
async function triggerTestSignal() {
  const r = await fetch('/api/egress/test-signal', {method: 'POST'});
  document.getElementById('controlmsg').textContent =
    r.ok ? 'impulse fired' : await r.text();
}
async function poll() {
  try {
    const r = await fetch('/api/status');
    const s = await r.json();
    const delayInput = document.getElementById('delay');
    if (document.activeElement !== delayInput) {
      delayInput.value = s.engine.egress_delay_ms;
    }
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
    if (s.pending_restart && s.pending_restart.length) {
      html = '<p class="bad">Configuration saved, waiting for a restart: ' +
        s.pending_restart.join(', ') + '</p>' + html;
    }
    document.getElementById('root').innerHTML = html;
  } catch (e) {
    document.getElementById('root').textContent = 'cannot reach the appliance: ' + e;
  }
}
poll();
setInterval(poll, 1000);
loadConfig();
</script>
</body>
</html>
)HTML";

}  // namespace

ApiServer::ApiServer(Config* config, Engine* engine, daemon::DaemonClient* daemon,
                     std::string webui_dir, std::string config_path)
    : config_(config),
      engine_(engine),
      daemon_(daemon),
      webui_dir_(std::move(webui_dir)),
      config_path_(std::move(config_path)) {}

ApiServer::~ApiServer() {
  stop();
}

Config ApiServer::config_snapshot() {
  std::lock_guard<std::mutex> lock(config_mutex_);
  return config_ != nullptr ? *config_ : Config{};
}

nlohmann::json ApiServer::build_preflight() {
  const EngineStatus status =
      engine_ != nullptr ? engine_->status() : EngineStatus{};
  const Config config = config_snapshot();

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
  checks.push_back({{"name", "link"},
                    {"ok", link_open},
                    {"detail", link_open ? config.link.mode + " " + config.link.role
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

  const Config config = config_snapshot();
  nlohmann::json document;
  document["name"] = "aes67-srt";
  document["version"] = version_string();
  document["build"] = build_info();
  document["role"] = config.link.role;
  document["mode"] = config.link.mode;
  document["peer"] = config.link.peer;
  document["blocks"] = config.blocks.size();
  document["channels"] = config.audio.channels;
  {
    std::lock_guard<std::mutex> lock(config_mutex_);
    // A configuration POST that needs a restart says so here, rather than the
    // change appearing to have taken effect.
    document["pending_restart"] = pending_restart_;
    document["config_path"] = config_path_;
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

  // ---- configuration (ticket 14, issue #15) ------------------------------
  // The running document, and a whole-document replace. A change is validated
  // before anything is written or applied, and a field that only takes effect on a
  // restart is reported rather than silently deferred.
  svr->Get(
      "/api/config", [this](const httplib::Request&, httplib::Response& response) {
        reply_json(response, nlohmann::json::parse(config_snapshot().to_json()));
      });

  svr->Post("/api/config", [this](const httplib::Request& request,
                                  httplib::Response& response) {
    if (config_ == nullptr) {
      reply_text(response, 503, "no configuration");
      return;
    }
    // Validate the whole document first: a refusal names the offending
    // field and changes nothing.
    Config parsed;
    std::string reason;
    if (!parse_config(request.body, &parsed, &reason)) {
      reply_text(response, 400, reason);
      return;
    }

    const Config running = config_snapshot();
    const nlohmann::json wanted = nlohmann::json::parse(parsed.to_json());
    const nlohmann::json current = nlohmann::json::parse(running.to_json());

    std::vector<std::string> applied;
    std::vector<std::string> restart;
    static const char* k_sections[] = {"audio",    "link",   "aes67_daemon",
                                       "egress",   "blocks", "http_addr",
                                       "http_port"};
    for (const char* section : k_sections) {
      if (!wanted.contains(section) || !current.contains(section)) {
        continue;
      }
      if (wanted[section] == current[section]) {
        continue;
      }
      if (std::string(section) == "egress" &&
          parsed.egress.test_signal_channel == running.egress.test_signal_channel) {
        // Only the A/V offset moved, and that one is live.
        applied.push_back("egress.delay_ms");
        continue;
      }
      restart.push_back(section);
    }

    // Apply the live field before writing, so a value the engine refuses
    // never reaches the file. The running config takes the new offset so
    // that file and running state agree on the field that did apply.
    if (!applied.empty()) {
      if (engine_ == nullptr ||
          !engine_->set_egress_delay_ms(parsed.egress.delay_ms, &reason)) {
        reply_text(response, 400, reason);
        return;
      }
    }

    if (!config_path_.empty()) {
      if (!write_atomically(config_path_, parsed.to_json(), &reason)) {
        reply_text(response, 500, reason);
        return;
      }
    }

    {
      std::lock_guard<std::mutex> lock(config_mutex_);
      if (!applied.empty()) {
        config_->egress.delay_ms = parsed.egress.delay_ms;
      }
      pending_restart_ = restart;
    }
    log().write(LogLevel::info,
                "control: configuration saved" +
                    (restart.empty() ? std::string()
                                     : " (" + std::to_string(restart.size()) +
                                           " section(s) need a restart)"));
    reply_json(response, {{"ok", true},
                          {"applied", applied},
                          {"restart_required", restart},
                          {"path", config_path_}});
  });

  // ---- A/V alignment, live (ticket 14, issue #15) ------------------------
  // The offset moves while audio runs, through the delay line's crossfade, and the
  // impulse is something to line the audio up against. Both are refused with the
  // field named, and neither blocks: the engine posts the request and the receive
  // loop applies it on its next period.
  svr->Post("/api/egress/delay", [this](const httplib::Request& request,
                                        httplib::Response& response) {
    std::string error;
    const nlohmann::json body = parse_body(request, &error);
    if (!error.empty()) {
      reply_text(response, 400, error);
      return;
    }
    if (!body.contains("delay_ms") || !body["delay_ms"].is_number()) {
      reply_text(response, 400, "egress.delay_ms: expected a number");
      return;
    }
    if (engine_ == nullptr) {
      reply_text(response, 503, "the engine is not running");
      return;
    }
    if (!engine_->set_egress_delay_ms(body["delay_ms"].get<double>(), &error)) {
      reply_text(response, 400, error);
      return;
    }
    reply_json(response, {{"ok", true}, {"delay_ms", engine_->egress_delay_ms()}});
  });

  svr->Post("/api/egress/test-signal",
            [this](const httplib::Request&, httplib::Response& response) {
              std::string error;
              if (engine_ == nullptr) {
                reply_text(response, 503, "the engine is not running");
                return;
              }
              if (!engine_->trigger_test_signal(&error)) {
                reply_text(response, 400, error);
                return;
              }
              reply_json(response, {{"ok", true}});
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
                webui_dir_.empty()
                    ? std::string("control: no web UI directory configured; "
                                  "serving the built-in status page")
                    : "control: no web UI at '" + webui_dir_ +
                          "'; serving the built-in status page");
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
