#include "audio/device_bridge.hpp"

#include <cstring>

namespace aes67_srt::audio {

void DeviceBridge::configure(unsigned channels) {
  channels_ = channels;
}

void DeviceBridge::bind(SharedRing* to_host, SharedRing* from_host) {
  to_host_ = to_host;
  from_host_ = from_host;
  // A bound ring knows its own channel count; take it when none was configured, so
  // a caller cannot bind without the pad being able to compute its length.
  if (channels_ == 0 && to_host != nullptr) {
    channels_ = to_host->channels();
  }
}

void DeviceBridge::unbind() {
  to_host_ = nullptr;
  from_host_ = nullptr;
}

size_t DeviceBridge::read_input(float* destination, size_t frames) {
  if (destination == nullptr || frames == 0) {
    return 0;
  }
  size_t delivered = 0;
  if (to_host_ != nullptr) {
    delivered = to_host_->read(destination, frames);
  }
  if (delivered < frames && channels_ != 0) {
    // Pad with silence. A hole is kinder than a repeat, which buzzes on tonal
    // material; the length of the pad is the ring's underrun count, and this class
    // reports it too so the page can show it without knowing about the ring. The
    // channel count comes from the device, so this holds even with no region bound.
    const size_t missing = frames - delivered;
    std::memset(destination + delivered * channels_, 0,
                missing * channels_ * sizeof(float));
    frames_silenced_.fetch_add(missing, std::memory_order_relaxed);
  }
  return delivered;
}

size_t DeviceBridge::write_output(const float* source, size_t frames) {
  if (source == nullptr || frames == 0 || from_host_ == nullptr) {
    return 0;
  }
  return from_host_->write(source, frames);
}

}  // namespace aes67_srt::audio