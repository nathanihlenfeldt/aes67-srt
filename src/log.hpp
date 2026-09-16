#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace aes67_srt {

enum class LogLevel { debug = 0, info = 1, warn = 2, error = 3 };

/**
 * A log that keeps the last N lines in memory as well as writing to stderr.
 *
 * The web UI tails the in-memory copy over the status endpoint.  A file is not
 * enough on an appliance in someone else's building: the person diagnosing it
 * is on the other end of a browser, and asking them to find a file is asking
 * them to fail.
 */
class Log {
 public:
  explicit Log(size_t capacity = 500);

  void set_level(LogLevel level);
  LogLevel level() const;

  void write(LogLevel level, const std::string& message);

  /** The retained lines, oldest first, as the UI would render them. */
  std::vector<std::string> tail(size_t lines) const;

 private:
  LogLevel level_ = LogLevel::info;
  size_t capacity_;
  std::vector<std::string> lines_;
  size_t next_ = 0;  // ring buffer write position
  bool wrapped_ = false;
};

/** Process-wide log, so modules do not have to pass one through every seam. */
Log& log();

std::string to_string(LogLevel level);

}  // namespace aes67_srt
