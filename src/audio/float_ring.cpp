#include "audio/float_ring.hpp"

#include <algorithm>
#include <cstring>

namespace aes67_srt::audio {

FloatRing::~FloatRing() = default;

bool FloatRing::open(size_t frames, unsigned channels) {
  if (frames < 2 || channels == 0) {
    // One frame is not a ring: "full" and "empty" would be the same state.
    return false;
  }
  buffer_.assign(frames * channels, 0.0f);
  capacity_frames_ = frames;
  channels_ = channels;
  write_index_.store(0);
  read_index_.store(0);
  frames_written_.store(0);
  frames_read_.store(0);
  frames_dropped_.store(0);
  frames_underrun_.store(0);
  return true;
}

void FloatRing::close() {
  buffer_.clear();
  buffer_.shrink_to_fit();
  capacity_frames_ = 0;
  channels_ = 0;
}

size_t FloatRing::write(const float* source, size_t frames) {
  if (buffer_.empty() || source == nullptr || frames == 0) {
    return 0;
  }
  const size_t write = write_index_.load(std::memory_order_relaxed);
  // Acquire: everything the consumer published before advancing its index must be
  // visible before this thread overwrites the storage it has just released.
  const size_t read = read_index_.load(std::memory_order_acquire);
  const size_t used = write - read;
  size_t room = capacity_frames_ - used;
  if (frames > room) {
    frames_dropped_.fetch_add(frames - room);
    frames = room;
  }
  if (frames == 0) {
    return 0;
  }
  const size_t start = write % capacity_frames_;
  const size_t first = std::min(frames, capacity_frames_ - start);
  std::memcpy(&buffer_[start * channels_], source,
              first * channels_ * sizeof(float));
  if (frames > first) {
    std::memcpy(&buffer_[0], source + first * channels_,
                (frames - first) * channels_ * sizeof(float));
  }
  // Release: the samples are in the buffer before the consumer can see the index.
  write_index_.store(write + frames, std::memory_order_release);
  frames_written_.fetch_add(frames);
  return frames;
}

size_t FloatRing::read(float* destination, size_t frames) {
  if (buffer_.empty() || destination == nullptr || frames == 0) {
    return 0;
  }
  // The consumer owns read_index_, so relaxed is enough for it; the producer's
  // write_index_ needs acquire to see the samples published before it.
  const size_t read = read_index_.load(std::memory_order_relaxed);
  const size_t write = write_index_.load(std::memory_order_acquire);
  const size_t available = write - read;
  if (frames > available) {
    frames_underrun_.fetch_add(frames - available);
    frames = available;
  }
  if (frames == 0) {
    return 0;
  }
  const size_t start = read % capacity_frames_;
  const size_t first = std::min(frames, capacity_frames_ - start);
  std::memcpy(destination, &buffer_[start * channels_],
              first * channels_ * sizeof(float));
  if (frames > first) {
    std::memcpy(destination + first * channels_, &buffer_[0],
                (frames - first) * channels_ * sizeof(float));
  }
  read_index_.store(read + frames, std::memory_order_release);
  frames_read_.fetch_add(frames);
  return frames;
}

size_t FloatRing::available_read() const {
  const size_t write = write_index_.load(std::memory_order_acquire);
  const size_t read = read_index_.load(std::memory_order_relaxed);
  return write - read;
}

size_t FloatRing::available_write() const {
  const size_t write = write_index_.load(std::memory_order_relaxed);
  const size_t read = read_index_.load(std::memory_order_acquire);
  return capacity_frames_ - (write - read);
}

}  // namespace aes67_srt::audio
