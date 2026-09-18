#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "config.hpp"

namespace aes67_srt {

class Engine;

/**
 * The engine's lifecycle, as the control surface sees it (issue #33).
 *
 * The control surface can *see* everything and, until this existed, could *do*
 * nothing: an operator at a site needs to start, stop and restart the audio from
 * the page, not walk to a terminal. This is the seam that makes the engine
 * stoppable and startable while the process — and therefore the page — stays up.
 */
class Service {
 public:
  virtual ~Service() = default;

  /** The running engine, or null when stopped. */
  virtual Engine* engine() = 0;

  /** True while the engine's loops are running. */
  virtual bool running() const = 0;

  /** One word for the page: stopped, starting, running, failed. */
  virtual std::string state() const = 0;

  /** Why the last start failed. Empty when none. */
  virtual std::string last_error() const = 0;

  /** Start the engine. Idempotent; returns once the worker is spawned. */
  virtual bool start(std::string* error) = 0;

  /** Stop the engine and wait for it. Prompt, because stop is interruptible. */
  virtual void stop() = 0;

  /** Stop, then start — which is how a saved configuration change is applied. */
  virtual bool restart(std::string* error) = 0;
};

/**
 * Owns the engine and the thread it runs on.
 *
 * `App::run` used to build the engine on the stack and run it for the life of the
 * process, so nothing could stop it. This owns it instead: `start` rebuilds it
 * from the current configuration and runs it on a worker; `stop` stops it and
 * joins; the engine is recreated each time, which is the simplest correct
 * lifecycle and is what lets a restart pick up a new configuration.
 *
 * **Start returns as soon as the worker is spawned, not when the link is up.** A
 * listener blocks in `accept`, so waiting for "up" would hang the HTTP request
 * that asked for it; the worker records what happened and the page polls.
 *
 * **Restart reloads the configuration file** — `POST /api/config` deliberately
 * keeps the saved configuration separate from the running one, so a restart is
 * exactly what turns `restart_required` into applied. The reload takes the shared
 * configuration lock, so it cannot race a status poll reading the same config.
 */
class EngineService : public Service {
 public:
  EngineService(Config* config, std::mutex* config_mutex, std::string config_path);
  ~EngineService() override;

  EngineService(const EngineService&) = delete;
  EngineService& operator=(const EngineService&) = delete;

  Engine* engine() override;
  bool running() const override;
  std::string state() const override;
  std::string last_error() const override;
  bool start(std::string* error) override;
  void stop() override;
  bool restart(std::string* error) override;

 private:
  void worker();

  Config* config_;
  std::mutex* config_mutex_;
  std::string config_path_;

  mutable std::mutex mutex_;
  std::unique_ptr<Engine> engine_;
  std::thread thread_;
  std::atomic<bool> running_{false};
  std::string state_{"stopped"};
  std::string error_;
};

}  // namespace aes67_srt
