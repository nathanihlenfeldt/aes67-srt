#include "audio/shared_ring.hpp"

#include <algorithm>
#include <cstring>
#include <new>

namespace aes67_srt::audio {

namespace {

size_t round_up(size_t value, size_t alignment) {
  return (value + alignment - 1) / alignment * alignment;
}

bool fail(std::string* error, const std::string& message) {
  if (error != nullptr) {
    *error = message;
  }
  return false;
}

void reset(SharedRingState& state) {
  state.write_index.store(0, std::memory_order_relaxed);
  state.read_index.store(0, std::memory_order_relaxed);
  state.frames_written.store(0, std::memory_order_relaxed);
  state.frames_read.store(0, std::memory_order_relaxed);
  state.frames_dropped.store(0, std::memory_order_relaxed);
  state.frames_underrun.store(0, std::memory_order_relaxed);
}

}  // namespace

size_t shared_data_offset() {
  return round_up(sizeof(SharedBlock), 64);
}

size_t shared_bytes_for(size_t capacity_frames, unsigned channels) {
  return shared_data_offset() + 2 * capacity_frames * channels * sizeof(float);
}

void SharedRing::attach(float* data, size_t capacity_frames, unsigned channels,
                        SharedRingState* state) {
  data_ = data;
  capacity_frames_ = capacity_frames;
  channels_ = channels;
  state_ = state;
}

void SharedRing::detach() {
  data_ = nullptr;
  capacity_frames_ = 0;
  channels_ = 0;
  state_ = nullptr;
}

size_t SharedRing::write(const float* source, size_t frames) {
  if (!is_attached() || source == nullptr || frames == 0) {
    return 0;
  }
  if (frames > capacity_frames_) {
    // More than a ring's worth in one offer. Keep the newest, as the full-ring rule
    // does: the older frames of the offer are dropped and counted, and what is
    // written is the last capacity's worth.
    const size_t excess = frames - capacity_frames_;
    state_->frames_dropped.fetch_add(excess, std::memory_order_relaxed);
    source += excess * channels_;
    frames = capacity_frames_;
  }
  const uint64_t write = state_->write_index.load(std::memory_order_relaxed);
  // Acquire: the consumer published its read index after freeing the storage, and
  // we must see that before we overwrite it.
  const uint64_t read = state_->read_index.load(std::memory_order_acquire);
  const uint64_t used = write - read;
  if (used + frames > capacity_frames_) {
    // Full, and the realtime producer's rule is to keep the newest: the frames the
    // consumer has not read are what gives way. Count them, so the loss is not
    // silent.
    const uint64_t overwritten = used + frames - capacity_frames_;
    state_->frames_dropped.fetch_add(overwritten, std::memory_order_relaxed);
  }
  const size_t start = static_cast<size_t>(write % capacity_frames_);
  const size_t first = std::min(frames, capacity_frames_ - start);
  std::memcpy(&data_[start * channels_], source, first * channels_ * sizeof(float));
  if (frames > first) {
    std::memcpy(&data_[0], source + first * channels_,
                (frames - first) * channels_ * sizeof(float));
  }
  // Release: the samples are in the storage before the consumer can see the index.
  state_->write_index.store(write + frames, std::memory_order_release);
  state_->frames_written.fetch_add(frames, std::memory_order_relaxed);
  return frames;
}

size_t SharedRing::read(float* destination, size_t frames) {
  if (!is_attached() || destination == nullptr || frames == 0) {
    return 0;
  }
  const uint64_t read = state_->read_index.load(std::memory_order_relaxed);
  // Acquire: the producer published its write index after copying the samples.
  const uint64_t write = state_->write_index.load(std::memory_order_acquire);
  uint64_t start_read = read;
  if (write - read > capacity_frames_) {
    // The producer lapped us: the oldest frames were overwritten and already
    // counted by the producer. Skip to the oldest frame that still exists. Only the
    // consumer writes read_index, so this stays single-writer per index.
    start_read = write - capacity_frames_;
  }
  const uint64_t available = write - start_read;
  size_t take = frames;
  if (take > available) {
    state_->frames_underrun.fetch_add(take - available, std::memory_order_relaxed);
    take = static_cast<size_t>(available);
  }
  if (take == 0) {
    if (start_read != read) {
      state_->read_index.store(start_read, std::memory_order_release);
    }
    return 0;
  }
  const size_t start = static_cast<size_t>(start_read % capacity_frames_);
  const size_t first = std::min(take, capacity_frames_ - start);
  std::memcpy(destination, &data_[start * channels_],
              first * channels_ * sizeof(float));
  if (take > first) {
    std::memcpy(destination + first * channels_, &data_[0],
                (take - first) * channels_ * sizeof(float));
  }
  // Release: the samples are copied out before the producer can overwrite them.
  state_->read_index.store(start_read + take, std::memory_order_release);
  state_->frames_read.fetch_add(take, std::memory_order_relaxed);
  return take;
}

size_t SharedRing::available_read() const {
  if (!is_attached()) {
    return 0;
  }
  const uint64_t write = state_->write_index.load(std::memory_order_acquire);
  const uint64_t read = state_->read_index.load(std::memory_order_relaxed);
  return static_cast<size_t>(std::min<uint64_t>(write - read, capacity_frames_));
}

size_t SharedRing::available_write() const {
  if (!is_attached()) {
    return 0;
  }
  const uint64_t write = state_->write_index.load(std::memory_order_relaxed);
  const uint64_t read = state_->read_index.load(std::memory_order_acquire);
  const uint64_t used = write - read;
  if (used >= capacity_frames_) {
    return 0;
  }
  return static_cast<size_t>(capacity_frames_ - used);
}

bool SharedRing::lapped() const {
  if (!is_attached()) {
    return false;
  }
  const uint64_t write = state_->write_index.load(std::memory_order_acquire);
  const uint64_t read = state_->read_index.load(std::memory_order_relaxed);
  return write - read > capacity_frames_;
}

uint64_t SharedRing::frames_written() const {
  return state_ != nullptr ? state_->frames_written.load(std::memory_order_relaxed)
                           : 0;
}

uint64_t SharedRing::frames_read() const {
  return state_ != nullptr ? state_->frames_read.load(std::memory_order_relaxed)
                           : 0;
}

uint64_t SharedRing::frames_dropped() const {
  return state_ != nullptr ? state_->frames_dropped.load(std::memory_order_relaxed)
                           : 0;
}

uint64_t SharedRing::frames_underrun() const {
  return state_ != nullptr ? state_->frames_underrun.load(std::memory_order_relaxed)
                           : 0;
}

bool SharedAudio::bind(void* region, size_t bytes, std::string* error) {
  auto* block = static_cast<SharedBlock*>(region);
  const size_t needed = shared_bytes_for(block->capacity_frames, block->channels);
  if (bytes < needed) {
    return fail(error, "shared audio region: too small for its own shape (need " +
                           std::to_string(needed) + " bytes, got " +
                           std::to_string(bytes) + ")");
  }
  auto* base = static_cast<char*>(region);
  auto* to_host_data = reinterpret_cast<float*>(base + shared_data_offset());
  auto* from_host_data = to_host_data + block->capacity_frames * block->channels;

  block_ = block;
  to_host_.attach(to_host_data, block->capacity_frames, block->channels,
                  &block->to_host);
  from_host_.attach(from_host_data, block->capacity_frames, block->channels,
                    &block->from_host);
  return true;
}

bool SharedAudio::create(void* region, size_t bytes, size_t capacity_frames,
                         unsigned channels, std::string* error) {
  if (region == nullptr) {
    return fail(error, "shared audio region: null");
  }
  if (capacity_frames < 2) {
    return fail(error,
                "shared audio capacity_frames: a ring needs at least two "
                "frames, got " +
                    std::to_string(capacity_frames));
  }
  if (channels == 0) {
    return fail(error, "shared audio channels: a ring needs at least one channel");
  }
  const size_t needed = shared_bytes_for(capacity_frames, channels);
  if (bytes < needed) {
    return fail(error, "shared audio region: too small for " +
                           std::to_string(capacity_frames) + " frames of " +
                           std::to_string(channels) + " channels (need " +
                           std::to_string(needed) + " bytes, got " +
                           std::to_string(bytes) + ")");
  }
  // Placement-new the header so the atomics are constructed where they lie, then
  // zero the counters. The buffers follow the header in the same region.
  auto* block = new (region) SharedBlock;
  block->magic = kSharedRingMagic;
  block->version = kSharedRingVersion;
  block->channels = channels;
  block->capacity_frames = static_cast<uint32_t>(capacity_frames);
  reset(block->to_host);
  reset(block->from_host);
  return bind(region, bytes, error);
}

bool SharedAudio::attach(void* region, size_t bytes, std::string* error) {
  if (region == nullptr) {
    return fail(error, "shared audio region: null");
  }
  if (bytes < sizeof(SharedBlock)) {
    return fail(error, "shared audio region: smaller than a header");
  }
  auto* block = static_cast<SharedBlock*>(region);
  if (block->magic != kSharedRingMagic) {
    return fail(error,
                "shared audio region: not an aes67-srt audio region "
                "(bad magic)");
  }
  if (block->version != kSharedRingVersion) {
    return fail(error, "shared audio version: the region is version " +
                           std::to_string(block->version) + ", this build speaks " +
                           std::to_string(kSharedRingVersion));
  }
  if (block->channels == 0 || block->capacity_frames < 2) {
    return fail(error, "shared audio shape: impossible (channels " +
                           std::to_string(block->channels) + ", capacity " +
                           std::to_string(block->capacity_frames) + ")");
  }
  return bind(region, bytes, error);
}

}  // namespace aes67_srt::audio