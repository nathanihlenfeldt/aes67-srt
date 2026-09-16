#pragma once

#include <string>

namespace aes67_srt {

/** The binary's semantic version, injected by CMake at configure time. */
const char* version_string();

/**
 * What this particular binary was built with, e.g. "srt=0 alsa=0".
 *
 * Reported by --version and by the status endpoint, because "it behaves
 * differently on the Pi" is almost always a build-flag difference rather than a
 * code difference, and the answer should not require a shell.
 */
std::string build_info();

}  // namespace aes67_srt
