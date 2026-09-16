#include "version.hpp"

namespace aes67_srt {

const char* version_string() {
  return AES67_SRT_VERSION;
}

std::string build_info() {
  std::string info;
  info += "srt=";
  info += (AES67_SRT_WITH_SRT ? "1" : "0");
  info += " alsa=";
  info += (AES67_SRT_WITH_ALSA ? "1" : "0");
#ifdef NDEBUG
  info += " assert=0";
#else
  info += " assert=1";
#endif
  return info;
}

}  // namespace aes67_srt
