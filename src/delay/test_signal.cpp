#include "delay/test_signal.hpp"

#include <cstring>

namespace aes67_srt::delay {
namespace {

bool fail(std::string* error, const std::string& message) {
  if (error != nullptr) {
    *error = message;
  }
  return false;
}

void encode_sample(uint8_t* at, unsigned sample_bytes, int32_t value) {
  at[0] = static_cast<uint8_t>(value & 0xff);
  at[1] = static_cast<uint8_t>((value >> 8) & 0xff);
  if (sample_bytes == 3) {
    at[2] = static_cast<uint8_t>((value >> 16) & 0xff);
  }
}

}  // namespace

TestSignal::TestSignal() = default;
TestSignal::~TestSignal() = default;

bool TestSignal::open(const Config& config, std::string* error) {
  if (config.channels == 0 || config.period_frames == 0) {
    return fail(error, "test signal: no channels or no period");
  }
  if (config.sample_bytes != 2 && config.sample_bytes != 3) {
    return fail(error, "test signal: sample_bytes must be 2 or 3");
  }
  if (config.channel < -1 || config.channel >= static_cast<int>(config.channels)) {
    return fail(error, "test signal: channel " + std::to_string(config.channel) +
                           " is not in 0.." + std::to_string(config.channels - 1));
  }
  if (config.max_pending == 0) {
    return fail(error, "test signal: max_pending must be at least 1");
  }
  config_ = config;
  open_ = true;
  queue_.clear();
  fired_ = 0;
  missed_ = 0;
  return true;
}

void TestSignal::close() {
  queue_.clear();
  queue_.shrink_to_fit();
  open_ = false;
}

bool TestSignal::is_open() const {
  return open_;
}

bool TestSignal::enabled() const {
  return open_ && config_.channel >= 0;
}

int TestSignal::channel() const {
  return config_.channel;
}

bool TestSignal::trigger(uint64_t at_frame, std::string* error) {
  if (!enabled()) {
    return fail(error,
                "test signal: no channel is selected (test_signal_channel "
                "is -1)");
  }
  return trigger(at_frame, config_.channel, error);
}

bool TestSignal::trigger(uint64_t at_frame, int channel, std::string* error) {
  if (!open_) {
    return fail(error, "test signal: not open");
  }
  if (channel < 0 || channel >= static_cast<int>(config_.channels)) {
    return fail(error, "test signal: channel " + std::to_string(channel) +
                           " is not in 0.." + std::to_string(config_.channels - 1));
  }
  if (queue_.size() >= config_.max_pending) {
    ++missed_;
    return fail(error, "test signal: " + std::to_string(config_.max_pending) +
                           " impulses are already queued; this one was dropped");
  }
  queue_.push_back(Pending{at_frame, channel});
  return true;
}

void TestSignal::mix(uint8_t* period, unsigned frames, uint64_t first_frame) {
  if (!open_ || period == nullptr || frames == 0) {
    return;
  }
  const size_t frame_bytes =
      static_cast<size_t>(config_.channels) * config_.sample_bytes;
  const int32_t full_scale = config_.sample_bytes == 3 ? 0x7FFFFF : 0x7FFF;
  size_t kept = 0;
  for (size_t index = 0; index < queue_.size(); ++index) {
    const Pending& pending = queue_[index];
    if (pending.frame >= first_frame &&
        pending.frame < first_frame + static_cast<uint64_t>(frames)) {
      const uint64_t offset = pending.frame - first_frame;
      uint8_t* at = period + offset * frame_bytes +
                    static_cast<size_t>(pending.channel) * config_.sample_bytes;
      encode_sample(at, config_.sample_bytes, full_scale);
      ++fired_;
    } else {
      queue_[kept++] = pending;
    }
  }
  queue_.resize(kept);
}

unsigned TestSignal::pending() const {
  return static_cast<unsigned>(queue_.size());
}

uint64_t TestSignal::fired() const {
  return fired_;
}

uint64_t TestSignal::missed() const {
  return missed_;
}

}  // namespace aes67_srt::delay
