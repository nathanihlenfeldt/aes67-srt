#include "service.hpp"

#include <chrono>

#include "config.hpp"
#include "engine.hpp"
#include "log.hpp"

namespace aes67_srt {

EngineService::EngineService(Config* config, std::mutex* config_mutex,
                             std::string config_path)
    : config_(config),
      config_mutex_(config_mutex),
      config_path_(std::move(config_path)) {}

EngineService::~EngineService() {
  stop();
}

Engine* EngineService::engine() {
  std::lock_guard<std::mutex> lock(mutex_);
  return engine_.get();
}

bool EngineService::running() const {
  return running_.load();
}

std::string EngineService::state() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return state_;
}

std::string EngineService::last_error() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return error_;
}

bool EngineService::start(std::string* error) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (thread_.joinable()) {
      return true;  // already running (or starting); starting again is a no-op
    }
    state_ = "starting";
    error_.clear();
  }

  // Reload the file so a restart applies a saved change. Validation refuses a
  // document that cannot run, and a refusal here changes nothing: the previous
  // configuration stays, and the reason is reported.
  if (!config_path_.empty() && config_ != nullptr && config_mutex_ != nullptr) {
    Config reloaded;
    std::string reason;
    if (load_config(config_path_, &reloaded, &reason)) {
      std::lock_guard<std::mutex> lock(*config_mutex_);
      *config_ = reloaded;
    } else {
      std::lock_guard<std::mutex> lock(mutex_);
      state_ = "failed";
      error_ = reason;
      if (error != nullptr) {
        *error = reason;
      }
      return false;
    }
  }

  // Snapshot the configuration under the shared lock, so a concurrent status poll
  // or config POST cannot change it under `prepare`.
  Config snapshot;
  {
    std::lock_guard<std::mutex> lock(*config_mutex_);
    snapshot = *config_;
  }

  auto engine = std::unique_ptr<Engine>(new Engine());
  std::string reason;
  if (!engine->prepare(snapshot, &reason)) {
    std::lock_guard<std::mutex> lock(mutex_);
    state_ = "failed";
    error_ = reason;
    if (error != nullptr) {
      *error = reason;
    }
    return false;
  }

  {
    std::lock_guard<std::mutex> lock(mutex_);
    engine_ = std::move(engine);
  }
  running_.store(true);
  thread_ = std::thread([this] { worker(); });
  return true;
}

void EngineService::worker() {
  Engine* engine = nullptr;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    engine = engine_.get();
    if (engine != nullptr) {
      // The loops are about to run. A listener may still be waiting for a peer,
      // but it is started, which is what "running" means here.
      state_ = "running";
    }
  }
  const int result = engine != nullptr ? engine->run() : 1;

  std::lock_guard<std::mutex> lock(mutex_);
  running_.store(false);
  // `run` returning normally means it was stopped; a non-zero result is a failure
  // that happened on the way (it logs its own reason).
  state_ = result == 0 ? "stopped" : "failed";
  if (result != 0) {
    error_ = "the engine stopped with an error; see the log";
  }
}

void EngineService::stop() {
  Engine* engine = nullptr;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!thread_.joinable()) {
      return;
    }
    engine = engine_.get();
  }
  if (engine != nullptr) {
    engine->stop();
  }
  if (thread_.joinable()) {
    thread_.join();
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    engine_.reset();
    running_.store(false);
    state_ = "stopped";
  }
}

bool EngineService::restart(std::string* error) {
  stop();
  return start(error);
}

}  // namespace aes67_srt