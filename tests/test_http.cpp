// The control surface's REST API (ticket 13, issue #14).
//
// The server is exercised over a real localhost socket rather than by calling
// build_status() directly, because "the page still loads and says exactly what is
// wrong" is a statement about an HTTP response, and the fallback page is served by
// the same routes the real one will be.

#include "http/api_server.hpp"

#include <chrono>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "aes67/daemon_client.hpp"
#include "config.hpp"
#include "engine.hpp"
#include "test_framework.hpp"

namespace {

using aes67_srt::ApiServer;
using aes67_srt::Config;
using aes67_srt::Engine;

Config http_config(int port, const std::string& mode) {
  Config config;
  config.audio.backend = "null";
  config.audio.channels = 8;
  config.link.blocks = 1;
  config.link.mode = mode;
  config.link.role = "duplex";
  config.link.local_port = port + 10;
  config.http_addr = "127.0.0.1";
  config.http_port = port;
  config.daemon.fake = true;
  config.fill_default_blocks();
  return config;
}

/** The server binds synchronously but accepts on its own thread; retry briefly. */
httplib::Result get_retrying(httplib::Client* client, const std::string& path) {
  for (int attempt = 0; attempt < 40; ++attempt) {
    httplib::Result result = client->Get(path);
    if (result && result->status != 0) {
      return result;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
  }
  return httplib::Result(nullptr, httplib::Error::Connection);
}

httplib::Result post_json(httplib::Client* client, const std::string& path,
                          const std::string& body) {
  return client->Post(path, body, "application/json");
}

}  // namespace

TEST_CASE(http_status_carries_the_engine_figures_and_the_preflight) {
  Config config = http_config(18211, "loopback");
  Engine engine;
  std::string error;
  CHECK(engine.prepare(config, &error));
  CHECK(engine.open(&error));
  auto daemon = aes67_srt::daemon::DaemonClient::create(config.daemon);

  ApiServer server(&config, &engine, daemon.get(), "/nonexistent-webui", "");
  CHECK(server.start(&error));

  httplib::Client client("127.0.0.1", config.http_port);
  httplib::Result response = get_retrying(&client, "/api/status");
  CHECK(response);
  CHECK_EQ(response->status, 200);

  const nlohmann::json body = nlohmann::json::parse(response->body);
  CHECK_EQ(body["name"].get<std::string>(), std::string("aes67-srt"));
  CHECK_EQ(body["role"].get<std::string>(), std::string("duplex"));
  CHECK(body.contains("engine"));
  CHECK(body["engine"].contains("delay_ms"));
  CHECK(body["engine"].contains("egress_delay_ms"));
  CHECK(body["engine"].contains("frames_sent"));

  // The preflight has four checks, PTP first, and with a locked fake daemon, an
  // open null device and an open loopback link it passes as a whole.
  CHECK(body.contains("preflight"));
  const nlohmann::json& preflight = body["preflight"];
  CHECK_EQ(preflight["checks"].size(), static_cast<size_t>(4));
  CHECK_EQ(preflight["checks"][0]["name"].get<std::string>(), std::string("ptp"));
  CHECK_EQ(preflight["checks"][0]["ok"].get<bool>(), true);
  CHECK_EQ(preflight["ok"].get<bool>(), true);

  server.stop();
  engine.stop();
}

TEST_CASE(http_status_says_exactly_what_is_wrong_with_no_daemon_and_no_link) {
  // The acceptance criterion: with neither a daemon nor a link, the page still
  // loads and names the failure rather than rendering nothing.
  Config config = http_config(18212, "listener");
  // A daemon client pointed at a closed port: unreachable, and it must be reported
  // as such rather than as PTP being unlocked, which is a different fix.
  config.daemon.fake = false;
  config.daemon.address = "127.0.0.1";
  config.daemon.port = 1;

  Engine engine;
  std::string error;
  CHECK(engine.prepare(config, &error));
  // Deliberately not opened: no device, no link.
  auto daemon = aes67_srt::daemon::DaemonClient::create(config.daemon);

  ApiServer server(&config, &engine, daemon.get(), "/nonexistent-webui", "");
  CHECK(server.start(&error));

  httplib::Client client("127.0.0.1", config.http_port);
  httplib::Result response = get_retrying(&client, "/api/status");
  CHECK(response);
  const nlohmann::json body = nlohmann::json::parse(response->body);

  CHECK_EQ(body["preflight"]["ok"].get<bool>(), false);
  // PTP leads, and it says it cannot know rather than guessing "unlocked".
  CHECK_EQ(body["preflight"]["checks"][0]["name"].get<std::string>(),
           std::string("ptp"));
  CHECK_EQ(body["preflight"]["checks"][0]["ok"].get<bool>(), false);
  CHECK(body["preflight"]["checks"][0]["detail"].get<std::string>().find(
            "daemon") != std::string::npos);
  // The daemon check names itself; device and link are down.
  bool daemon_failed = false;
  bool device_failed = false;
  bool link_failed = false;
  for (const nlohmann::json& check : body["preflight"]["checks"]) {
    const std::string name = check["name"].get<std::string>();
    if (name == "daemon" && !check["ok"].get<bool>()) {
      daemon_failed = true;
    }
    if (name == "device" && !check["ok"].get<bool>()) {
      device_failed = true;
    }
    if (name == "link" && !check["ok"].get<bool>()) {
      link_failed = true;
    }
  }
  CHECK(daemon_failed);
  CHECK(device_failed);
  CHECK(link_failed);

  // And the page is there, not a 404.
  httplib::Result page = get_retrying(&client, "/");
  CHECK(page);
  CHECK_EQ(page->status, 200);
  CHECK(page->body.find("aes67-srt") != std::string::npos);

  // A control request that cannot work is refused, naming the field.
  httplib::Result impulse = client.Post("/api/egress/test-signal");
  CHECK(impulse);
  CHECK_EQ(impulse->status, 400);
  CHECK(impulse->body.find("test_signal_channel") != std::string::npos);

  server.stop();
  engine.stop();
}

TEST_CASE(http_the_av_delay_is_adjustable_live_and_refused_when_impossible) {
  Config config = http_config(18214, "loopback");
  config.egress.test_signal_channel = 2;
  Engine engine;
  std::string error;
  CHECK(engine.prepare(config, &error));
  CHECK(engine.open(&error));
  auto daemon = aes67_srt::daemon::DaemonClient::create(config.daemon);

  ApiServer server(&config, &engine, daemon.get(), "/nonexistent-webui", "");
  CHECK(server.start(&error));
  httplib::Client client("127.0.0.1", config.http_port);
  // Warm the server up so the POST below is not racing its accept thread.
  CHECK(get_retrying(&client, "/api/status"));

  // A reasonable offset is accepted and shows up in the status.
  httplib::Result set =
      post_json(&client, "/api/egress/delay", R"({"delay_ms":250})");
  CHECK(set);
  CHECK_EQ(set->status, 200);
  httplib::Result status = get_retrying(&client, "/api/status");
  CHECK(status);
  const nlohmann::json body = nlohmann::json::parse(status->body);
  CHECK_NEAR(body["engine"]["egress_delay_ms"].get<double>(), 250.0, 0.05);

  // Impossible offsets are refused *before* anything is applied, naming the field.
  httplib::Result negative =
      post_json(&client, "/api/egress/delay", R"({"delay_ms":-1})");
  CHECK_EQ(negative->status, 400);
  CHECK(negative->body.find("egress.delay_ms") != std::string::npos);
  httplib::Result beyond =
      post_json(&client, "/api/egress/delay", R"({"delay_ms":6000})");
  CHECK_EQ(beyond->status, 400);
  CHECK(beyond->body.find("egress.delay_ms") != std::string::npos);
  httplib::Result wrong_type =
      post_json(&client, "/api/egress/delay", R"({"delay_ms":"soon"})");
  CHECK_EQ(wrong_type->status, 400);
  CHECK(wrong_type->body.find("expected a number") != std::string::npos);
  // And the good value is still what is applied: a refusal changed nothing.
  CHECK_NEAR(engine.egress_delay_ms(), 250.0, 0.05);

  // The test signal fires on the selected channel.
  httplib::Result impulse = client.Post("/api/egress/test-signal");
  CHECK(impulse);
  CHECK_EQ(impulse->status, 200);

  server.stop();
  engine.stop();
}

TEST_CASE(http_the_config_is_restart_aware_and_persisted) {
  // The configuration from the browser: validated before it is written, persisted
  // to the file it came from, and honest about what only takes effect on a restart.
  const std::string path = "/tmp/aes67-srt-http-config-test.json";
  std::remove(path.c_str());

  Config config = http_config(18215, "loopback");
  {
    std::ofstream out(path, std::ios::trunc);
    out << config.to_json();
  }

  Engine engine;
  std::string error;
  CHECK(engine.prepare(config, &error));
  CHECK(engine.open(&error));
  auto daemon = aes67_srt::daemon::DaemonClient::create(config.daemon);

  ApiServer server(&config, &engine, daemon.get(), "/nonexistent-webui", path);
  CHECK(server.start(&error));
  httplib::Client client("127.0.0.1", config.http_port);
  CHECK(get_retrying(&client, "/api/status"));

  // The running document, with the SRT fields the page will edit.
  httplib::Result got = get_retrying(&client, "/api/config");
  CHECK(got);
  CHECK_EQ(got->status, 200);
  nlohmann::json document = nlohmann::json::parse(got->body);
  CHECK_EQ(document["link"]["mode"].get<std::string>(), std::string("loopback"));
  CHECK(document["link"].contains("latency_ms"));
  CHECK(document["link"].contains("peer"));

  // A restart-only change: persisted, and reported as needing a restart.
  document["link"]["latency_ms"] = 200;
  httplib::Result saved = post_json(&client, "/api/config", document.dump());
  CHECK(saved);
  CHECK_EQ(saved->status, 200);
  nlohmann::json saved_body = nlohmann::json::parse(saved->body);
  CHECK_EQ(saved_body["restart_required"].size(), static_cast<size_t>(1));
  CHECK_EQ(saved_body["restart_required"][0].get<std::string>(),
           std::string("link"));
  {
    std::ifstream in(path);
    std::stringstream text;
    text << in.rdbuf();
    CHECK_EQ(nlohmann::json::parse(text.str())["link"]["latency_ms"].get<int>(),
             200);
  }

  // A live change: the offset only, applied now and needing no restart.
  document["link"]["latency_ms"] = 120;
  document["egress"]["delay_ms"] = 33.0;
  httplib::Result live = post_json(&client, "/api/config", document.dump());
  CHECK(live);
  CHECK_EQ(live->status, 200);
  nlohmann::json live_body = nlohmann::json::parse(live->body);
  CHECK_EQ(live_body["restart_required"].size(), static_cast<size_t>(0));
  CHECK_EQ(live_body["applied"].size(), static_cast<size_t>(1));
  CHECK_NEAR(engine.egress_delay_ms(), 33.0, 0.05);

  // An impossible document is refused naming the field, and changes nothing.
  document["link"]["latency_ms"] = 0;
  httplib::Result impossible = post_json(&client, "/api/config", document.dump());
  CHECK_EQ(impossible->status, 400);
  CHECK(impossible->body.find("latency_ms") != std::string::npos);

  // An unknown key is an error, not something silently ignored.
  document["link"]["latency_ms"] = 120;
  document["link"]["nonsense"] = 1;
  httplib::Result unknown = post_json(&client, "/api/config", document.dump());
  CHECK_EQ(unknown->status, 400);
  CHECK(unknown->body.find("nonsense") != std::string::npos);

  server.stop();
  engine.stop();
  std::remove(path.c_str());
}

TEST_CASE(http_version_and_log_endpoints_answer) {
  Config config = http_config(18213, "loopback");
  Engine engine;
  std::string error;
  CHECK(engine.prepare(config, &error));
  auto daemon = aes67_srt::daemon::DaemonClient::create(config.daemon);

  ApiServer server(&config, &engine, daemon.get(), "/nonexistent-webui", "");
  CHECK(server.start(&error));

  httplib::Client client("127.0.0.1", config.http_port);
  httplib::Result version = get_retrying(&client, "/api/version");
  CHECK(version);
  CHECK_EQ(version->status, 200);
  CHECK(nlohmann::json::parse(version->body).contains("build"));

  httplib::Result log = get_retrying(&client, "/api/log?lines=10");
  CHECK(log);
  CHECK_EQ(log->status, 200);
  CHECK(nlohmann::json::parse(log->body).contains("lines"));

  // An unknown API path is a 404, not the SPA page: a client asking for a route
  // that does not exist should be told, not handed HTML.
  httplib::Result missing = get_retrying(&client, "/api/nope");
  CHECK(missing);
  CHECK_EQ(missing->status, 404);

  server.stop();
}
