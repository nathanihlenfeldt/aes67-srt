#include "log.hpp"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <iostream>
#include <mutex>

namespace aes67_srt {
namespace {

std::mutex& log_mutex() {
  static std::mutex mutex;
  return mutex;
}

std::string timestamp() {
  const auto now = std::chrono::system_clock::now();
  const std::time_t seconds = std::chrono::system_clock::to_time_t(now);
  const auto millis =
      std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch())
          .count() %
      1000;
  std::tm tm{};
  localtime_r(&seconds, &tm);
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "%02d:%02d:%02d.%03d", tm.tm_hour,
                tm.tm_min, tm.tm_sec, static_cast<int>(millis));
  return buffer;
}

}  // namespace

std::string to_string(LogLevel level) {
  switch (level) {
    case LogLevel::debug:
      return "debug";
    case LogLevel::info:
      return "info";
    case LogLevel::warn:
      return "warn";
    case LogLevel::error:
      return "error";
  }
  return "info";
}

Log::Log(size_t capacity) : capacity_(capacity == 0 ? 1 : capacity) {
  lines_.resize(capacity_);
}

void Log::set_level(LogLevel level) {
  level_ = level;
}

LogLevel Log::level() const {
  return level_;
}

void Log::write(LogLevel level, const std::string& message) {
  std::lock_guard<std::mutex> guard(log_mutex());

  const std::string line = timestamp() + " " + to_string(level) + " " + message;
  lines_[next_] = line;
  next_ = (next_ + 1) % capacity_;
  if (next_ == 0) {
    wrapped_ = true;
  }

  if (level < level_) {
    return;
  }
  std::ostream& out = (level >= LogLevel::warn) ? std::cerr : std::cout;
  out << line << std::endl;
}

std::vector<std::string> Log::tail(size_t lines) const {
  std::lock_guard<std::mutex> guard(log_mutex());

  const size_t held = wrapped_ ? capacity_ : next_;
  const size_t want = std::min(lines, held);
  std::vector<std::string> result;
  result.reserve(want);
  // Walk backwards from the most recent write so a partially filled ring and a
  // wrapped one are handled by the same code.
  for (size_t i = 0; i < want; ++i) {
    const size_t index = (next_ + capacity_ - 1 - i) % capacity_;
    result.push_back(lines_[index]);
  }
  std::reverse(result.begin(), result.end());
  return result;
}

Log& log() {
  static Log instance;
  return instance;
}

}  // namespace aes67_srt
