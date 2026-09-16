#pragma once

#include <string>
#include <vector>

namespace aes67_srt {

/** Whitespace at both ends removed. */
std::string trim(const std::string& text);

/** ASCII lowercase. Used to accept "Duplex" as "duplex" in config. */
std::string to_lower(const std::string& text);

/** Split on a single character. Empty fields are kept: that is a config error. */
std::vector<std::string> split(const std::string& text, char separator);

/**
 * Split "host:port" into its parts.
 *
 * Returns false when the port is missing, not a number, or out of range, so the
 * caller can report which field was wrong rather than defaulting past it.
 */
bool split_host_port(const std::string& text, std::string* host, int* port);

}  // namespace aes67_srt
